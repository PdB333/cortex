#include "ocr.h"

#include <windows.h>
#include <shlwapi.h>
#include <bcrypt.h>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <cstdint>

namespace ocr {

namespace {

// PowerShell 5.1+ can drive WinRT directly via the RuntimeType/ActivationFactory
// bridge. The script:
//   * loads a PNG from disk into a Windows.Graphics.Imaging SoftwareBitmap,
//   * invokes OcrEngine.RecognizeAsync,
//   * emits a compact JSON blob on stdout with lines + word bounding boxes.
// Written to %TEMP%\cortex_ocr.ps1 on first use and reused on subsequent
// calls -- the ps1 body is content-addressed by process, no version churn.
constexpr const char* kPowerShellScript = R"POWERSHELL(
param([Parameter(Mandatory=$true)][string]$ImagePath, [string]$Language = "")

$ErrorActionPreference = "Stop"
try {
    [void][Windows.Storage.StorageFile,Windows.Storage,ContentType=WindowsRuntime]
    [void][Windows.Graphics.Imaging.BitmapDecoder,Windows.Graphics.Imaging,ContentType=WindowsRuntime]
    [void][Windows.Graphics.Imaging.SoftwareBitmap,Windows.Graphics.Imaging,ContentType=WindowsRuntime]
    [void][Windows.Media.Ocr.OcrEngine,Windows.Media.Ocr,ContentType=WindowsRuntime]
    [void][Windows.Globalization.Language,Windows.Globalization,ContentType=WindowsRuntime]
    Add-Type -AssemblyName System.Runtime.WindowsRuntime | Out-Null

    # Grab AsTask<TResult>(IAsyncOperation<TResult>) once via reflection.
    $asTaskGeneric = [System.WindowsRuntimeSystemExtensions].GetMethods() |
        Where-Object {
            $_.Name -eq 'AsTask' -and
            $_.GetParameters().Count -eq 1 -and
            $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1'
        } | Select-Object -First 1

    function Await($op, $resultType) {
        $method = $asTaskGeneric.MakeGenericMethod($resultType)
        $task   = $method.Invoke($null, @($op))
        $task.Wait(-1) | Out-Null
        $task.Result
    }

    $file    = Await ([Windows.Storage.StorageFile]::GetFileFromPathAsync($ImagePath)) ([Windows.Storage.StorageFile])
    $stream  = Await ($file.OpenAsync(0)) ([Windows.Storage.Streams.IRandomAccessStream])
    $decoder = Await ([Windows.Graphics.Imaging.BitmapDecoder]::CreateAsync($stream)) ([Windows.Graphics.Imaging.BitmapDecoder])
    $bitmap  = Await ($decoder.GetSoftwareBitmapAsync()) ([Windows.Graphics.Imaging.SoftwareBitmap])

    if ($Language -and $Language.Length -gt 0) {
        $langObj = [Windows.Globalization.Language]::new($Language)
        if (-not [Windows.Media.Ocr.OcrEngine]::IsLanguageSupported($langObj)) {
            $out = @{ ok = $false; error = "language_unsupported"; language = $Language }
            $out | ConvertTo-Json -Compress
            exit 2
        }
        $engine = [Windows.Media.Ocr.OcrEngine]::TryCreateFromLanguage($langObj)
    } else {
        $engine = [Windows.Media.Ocr.OcrEngine]::TryCreateFromUserProfileLanguages()
    }
    if (-not $engine) {
        @{ ok = $false; error = "no_ocr_engine_installed" } | ConvertTo-Json -Compress
        exit 3
    }

    $result = Await ($engine.RecognizeAsync($bitmap)) ([Windows.Media.Ocr.OcrResult])

    $lines = @()
    foreach ($ln in $result.Lines) {
        $words = @()
        foreach ($w in $ln.Words) {
            $words += @{
                text = $w.Text
                x = [double]$w.BoundingRect.X
                y = [double]$w.BoundingRect.Y
                w = [double]$w.BoundingRect.Width
                h = [double]$w.BoundingRect.Height
            }
        }
        $lines += @{ text = $ln.Text; words = $words }
    }

    @{
        ok = $true
        text = $result.Text
        text_angle = if ($result.TextAngle) { [double]$result.TextAngle } else { $null }
        lines = $lines
    } | ConvertTo-Json -Compress -Depth 8
} catch {
    @{ ok = $false; error = "exception"; message = "$_" } | ConvertTo-Json -Compress
    exit 1
}
)POWERSHELL";

std::string TempDir() {
    char buf[MAX_PATH] = {};
    DWORD n = GetTempPathA(MAX_PATH, buf);
    if (!n) return ".";
    std::string s(buf, n);
    while (!s.empty() && (s.back() == '\\' || s.back() == '/')) s.pop_back();
    return s;
}

std::string RandomHex(size_t bytes) {
    std::vector<uint8_t> buf(bytes);
    BCryptGenRandom(nullptr, buf.data(), (ULONG)buf.size(), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    static const char* h = "0123456789abcdef";
    std::string out; out.reserve(bytes * 2);
    for (uint8_t b : buf) { out.push_back(h[b >> 4]); out.push_back(h[b & 0xF]); }
    return out;
}

bool WriteFileBinary(const std::string& path, const void* data, size_t size) {
    HANDLE h = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    bool ok = WriteFile(h, data, (DWORD)size, &w, nullptr) && w == size;
    CloseHandle(h);
    return ok;
}

std::string EnsureScriptFile() {
    std::string path = TempDir() + "\\cortex_ocr.ps1";
    // If the script already matches, reuse it -- avoids racing multiple
    // concurrent calls that each try to rewrite the file.
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER sz{};
        GetFileSizeEx(h, &sz);
        CloseHandle(h);
        if ((size_t)sz.QuadPart == std::strlen(kPowerShellScript)) {
            return path;
        }
    }
    WriteFileBinary(path, kPowerShellScript, std::strlen(kPowerShellScript));
    return path;
}

// Runs a command with args, captures stdout up to a hard byte cap.
bool RunAndCapture(const std::string& cmdLine, std::string& stdoutOut,
                   DWORD timeoutMs, DWORD* exitCode) {
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = wr;
    si.hStdError  = wr;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};

    std::vector<char> mut(cmdLine.begin(), cmdLine.end());
    mut.push_back(0);

    BOOL ok = CreateProcessA(nullptr, mut.data(), nullptr, nullptr, TRUE,
                              CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (!ok) { CloseHandle(rd); return false; }

    // Drain stdout until the pipe closes (child exits) or timeout hits.
    ULONGLONG start = GetTickCount64();
    constexpr size_t kMaxOut = 8 * 1024 * 1024;
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr)) break;
        if (avail > 0) {
            char buf[4096];
            DWORD toRead = avail > sizeof(buf) ? sizeof(buf) : avail;
            DWORD gotten = 0;
            if (!ReadFile(rd, buf, toRead, &gotten, nullptr) || gotten == 0) break;
            if (stdoutOut.size() < kMaxOut) {
                size_t room = kMaxOut - stdoutOut.size();
                stdoutOut.append(buf, buf + (gotten > room ? room : gotten));
            }
            continue;
        }
        if (WaitForSingleObject(pi.hProcess, 20) == WAIT_OBJECT_0) {
            // Drain any tail bytes.
            char buf[4096]; DWORD gotten = 0;
            while (ReadFile(rd, buf, sizeof(buf), &gotten, nullptr) && gotten > 0) {
                if (stdoutOut.size() < kMaxOut) {
                    size_t room = kMaxOut - stdoutOut.size();
                    stdoutOut.append(buf, buf + (gotten > room ? room : gotten));
                }
            }
            break;
        }
        if (GetTickCount64() - start > timeoutMs) {
            TerminateProcess(pi.hProcess, 1);
            break;
        }
    }
    if (exitCode) GetExitCodeProcess(pi.hProcess, exitCode);
    CloseHandle(rd);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

std::string QuoteArg(const std::string& a) {
    std::string s = "\"";
    for (char c : a) { if (c == '"') s.push_back('\\'); s.push_back(c); }
    s.push_back('"');
    return s;
}

} // namespace

std::vector<uint8_t> Base64Decode(const std::string& s) {
    static int8_t T[256];
    static bool inited = false;
    if (!inited) {
        for (int i = 0; i < 256; ++i) T[i] = -1;
        const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) T[(unsigned char)a[i]] = (int8_t)i;
        inited = true;
    }
    std::vector<uint8_t> out;
    out.reserve(s.size() * 3 / 4);
    int val = 0, valb = -8;
    for (unsigned char c : s) {
        if (c == '=' || std::isspace(c)) { if (c == '=') break; else continue; }
        if (T[c] < 0) continue;
        val = (val << 6) | T[c];
        valb += 6;
        if (valb >= 0) { out.push_back((uint8_t)((val >> valb) & 0xFF)); valb -= 8; }
    }
    return out;
}

OcrResult Recognize(const std::vector<uint8_t>& png, const std::string& language) {
    OcrResult r{false, "", "", "winrt", ""};
    if (png.empty()) { r.error = "empty_image"; return r; }

    const std::string pngPath = TempDir() + "\\cortex_ocr_" + RandomHex(8) + ".png";
    if (!WriteFileBinary(pngPath, png.data(), png.size())) {
        r.error = "temp_write_failed"; return r;
    }
    const std::string ps1 = EnsureScriptFile();

    std::string cmd = "powershell.exe -NoProfile -ExecutionPolicy Bypass -File ";
    cmd += QuoteArg(ps1);
    cmd += " -ImagePath " + QuoteArg(pngPath);
    if (!language.empty()) cmd += " -Language " + QuoteArg(language);

    std::string out;
    DWORD ec = 0;
    bool ran = RunAndCapture(cmd, out, 30000, &ec);
    DeleteFileA(pngPath.c_str());
    if (!ran) { r.error = "spawn_failed"; return r; }

    // Trim leading/trailing whitespace/BOM so downstream JSON parsers don't
    // trip on PowerShell UTF-8 preambles.
    size_t start = 0;
    while (start < out.size() && (unsigned char)out[start] <= 0x20) ++start;
    size_t end = out.size();
    while (end > start && (unsigned char)out[end - 1] <= 0x20) --end;
    if (end - start >= 3 &&
        (unsigned char)out[start]   == 0xEF &&
        (unsigned char)out[start+1] == 0xBB &&
        (unsigned char)out[start+2] == 0xBF) start += 3;
    r.json.assign(out, start, end - start);

    if (r.json.empty()) { r.error = "no_output"; return r; }

    // Cheap extraction of the top-level "text" field without dragging json.hpp
    // into this TU; the full JSON is passed through anyway.
    const std::string key = "\"text\":";
    size_t p = r.json.find(key);
    if (p != std::string::npos) {
        p += key.size();
        while (p < r.json.size() && (r.json[p] == ' ' || r.json[p] == '\t')) ++p;
        if (p < r.json.size() && r.json[p] == '"') {
            ++p;
            std::string val;
            while (p < r.json.size() && r.json[p] != '"') {
                if (r.json[p] == '\\' && p + 1 < r.json.size()) {
                    char n = r.json[p + 1];
                    if      (n == 'n') val.push_back('\n');
                    else if (n == 't') val.push_back('\t');
                    else if (n == 'r') val.push_back('\r');
                    else                val.push_back(n);
                    p += 2;
                } else {
                    val.push_back(r.json[p++]);
                }
            }
            r.text = std::move(val);
        }
    }
    r.ok = ec == 0 && r.json.find("\"ok\":true") != std::string::npos;
    if (!r.ok && r.error.empty()) r.error = "ocr_failed";
    return r;
}

} // namespace ocr
