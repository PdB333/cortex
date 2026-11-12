#include "updater.h"

#include "../artifacts/artifact.h"
#include "../artifacts/sha256.h"
#include "../artifacts/zip_provider.h"
#include "../version.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <winhttp.h>
#endif

namespace cortex::setup {
namespace {

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool EndsWith(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool IsHttp(const std::string& url) {
    return url.rfind("https://", 0) == 0 || url.rfind("http://", 0) == 0;
}

std::string FileSha256(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    artifacts::Sha256 hash;
    std::vector<char> buffer(1 << 16);
    while (file) {
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        if (file.gcount() > 0) hash.Update(buffer.data(), static_cast<size_t>(file.gcount()));
    }
    if (file.bad()) return {};
    return artifacts::Sha256::Hex(hash.Final());
}

#if defined(_WIN32)
std::wstring Wide(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

std::string Narrow(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

// One GET over WinHTTP (system proxy settings, TLS validation by Windows).
// `sink` receives the body in chunks; it returns false to abort.
bool HttpGet(const std::string& url, const std::function<bool(const char*, size_t)>& sink, uint64_t& total,
             std::string& error) {
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256] = {};
    wchar_t path[2048] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 2048;
    wchar_t extra[2048] = {};
    parts.lpszExtraInfo = extra;
    parts.dwExtraInfoLength = 2048;
    const std::wstring wideUrl = Wide(url);
    if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &parts)) {
        error = "invalid_url";
        return false;
    }
    if (parts.nScheme != INTERNET_SCHEME_HTTPS) {
        error = "only https is accepted";
        return false;
    }
    const std::wstring agent = Wide(std::string("Cortex-Updater/") + cortex::kVersion);
    HINTERNET session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
        session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        error = "winhttp_open_failed:" + std::to_string(GetLastError());
        return false;
    }
    WinHttpSetTimeouts(session, 10000, 10000, 20000, 60000);
    bool ok = false;
    HINTERNET connection = WinHttpConnect(session, host, parts.nPort, 0);
    HINTERNET request = nullptr;
    if (connection) {
        const std::wstring object = std::wstring(path) + extra;
        request = WinHttpOpenRequest(connection, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    }
    if (request) {
        const wchar_t* headers = L"Accept: application/vnd.github+json, application/octet-stream, */*\r\n";
        if (WinHttpSendRequest(request, headers, static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(request, nullptr)) {
            DWORD status = 0;
            DWORD size = sizeof(status);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                &status, &size, WINHTTP_NO_HEADER_INDEX);
            wchar_t length[32] = {};
            DWORD lengthSize = sizeof(length);
            if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, length,
                                    &lengthSize, WINHTTP_NO_HEADER_INDEX))
                total = static_cast<uint64_t>(_wtoi64(length));
            if (status != 200) {
                error = "http_status_" + std::to_string(status);
            } else {
                ok = true;
                std::vector<char> buffer(1 << 16);
                for (;;) {
                    DWORD read = 0;
                    if (!WinHttpReadData(request, buffer.data(), static_cast<DWORD>(buffer.size()), &read)) {
                        error = "read_failed:" + std::to_string(GetLastError());
                        ok = false;
                        break;
                    }
                    if (read == 0) break;
                    if (!sink(buffer.data(), read)) {
                        ok = false;
                        if (error.empty()) error = "too_large";
                        break;
                    }
                }
            }
        } else {
            error = "request_failed:" + std::to_string(GetLastError());
        }
    } else if (error.empty()) {
        error = "connect_failed:" + std::to_string(GetLastError());
    }
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return ok;
}
#endif

}  // namespace

const ReleaseAsset* Release::Find(const std::string& suffix) const {
    for (const auto& asset : assets)
        if (EndsWith(Lower(asset.name), Lower(suffix))) return &asset;
    return nullptr;
}

int CompareVersions(const std::string& left, const std::string& right) {
    auto split = [](std::string text, std::vector<long>& numbers, std::string& pre) {
        if (!text.empty() && (text[0] == 'v' || text[0] == 'V')) text.erase(0, 1);
        const size_t dash = text.find('-');
        if (dash != std::string::npos) {
            pre = text.substr(dash + 1);
            text = text.substr(0, dash);
        }
        std::stringstream parts(text);
        std::string part;
        while (std::getline(parts, part, '.')) numbers.push_back(std::atol(part.c_str()));
    };
    std::vector<long> a, b;
    std::string preA, preB;
    split(left, a, preA);
    split(right, b, preB);
    const size_t count = std::max(a.size(), b.size());
    for (size_t i = 0; i < count; ++i) {
        const long x = i < a.size() ? a[i] : 0;
        const long y = i < b.size() ? b[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    if (preA == preB) return 0;
    if (preA.empty()) return 1;   // a release is newer than its pre-release
    if (preB.empty()) return -1;
    return preA < preB ? -1 : 1;
}

bool ParseRelease(const json& document, Release& release, std::string& error) {
    if (!document.is_object() || !document.contains("tag_name") || !document["tag_name"].is_string()) {
        error = "the feed is not a release document";
        return false;
    }
    auto text = [&document](const char* key) {
        const auto it = document.find(key);
        return it != document.end() && it->is_string() ? it->get<std::string>() : std::string();
    };
    release.tag = text("tag_name");
    release.version = release.tag;
    if (!release.version.empty() && (release.version[0] == 'v' || release.version[0] == 'V')) release.version.erase(0, 1);
    release.name = text("name");
    release.notes = text("body");
    release.page = text("html_url");
    release.assets.clear();
    const auto assets = document.find("assets");
    if (assets != document.end() && assets->is_array()) {
        for (const auto& asset : *assets) {
            if (!asset.is_object()) continue;
            ReleaseAsset item;
            if (asset.contains("name") && asset["name"].is_string()) item.name = asset["name"];
            if (asset.contains("browser_download_url") && asset["browser_download_url"].is_string())
                item.url = asset["browser_download_url"];
            if (asset.contains("size") && asset["size"].is_number_unsigned()) item.size = asset["size"];
            if (!item.name.empty() && !item.url.empty()) release.assets.push_back(item);
        }
    }
    return true;
}

std::string ChecksumFor(const std::string& sums, const std::string& fileName) {
    std::istringstream lines(sums);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream words(line);
        std::string hash, name;
        if (!(words >> hash >> name)) continue;
        if (!name.empty() && name[0] == '*') name.erase(0, 1);  // binary-mode marker of sha256sum
        if (name != fileName || hash.size() != 64) continue;
        if (!std::all_of(hash.begin(), hash.end(), [](unsigned char c) { return std::isxdigit(c) != 0; })) continue;
        return Lower(hash);
    }
    return {};
}

InstallKind DetectInstallKind(const fs::path& applicationDirectory) {
    std::error_code ec;
    // Inno Setup leaves its uninstaller beside the program.
    if (fs::exists(applicationDirectory / "unins000.exe", ec)) return InstallKind::Installed;
    return InstallKind::Portable;
}

const char* InstallKindName(InstallKind kind) { return kind == InstallKind::Installed ? "installed" : "portable"; }

std::string AssetSuffix(InstallKind kind) {
    return kind == InstallKind::Installed ? "-windows-setup.exe" : "-windows-portable.zip";
}

json UpdateCheck::ToJson() const {
    json out = {{"ok", ok}, {"current", current}, {"available", available}};
    if (!error.empty()) out["error"] = error;
    if (ok) {
        json assets = json::array();
        for (const auto& asset : latest.assets) assets.push_back({{"name", asset.name}, {"size", asset.size}});
        out["latest"] = {{"version", latest.version}, {"tag", latest.tag}, {"name", latest.name},
                         {"page", latest.page}, {"assets", assets}};
    }
    return out;
}

json PreparedUpdate::ToJson() const {
    json out = {{"ok", ok}, {"kind", InstallKindName(kind)}};
    if (!error.empty()) out["error"] = error;
    if (ok) {
        out["file"] = file.u8string();
        out["sha256"] = sha256;
    }
    return out;
}

bool FetchText(const std::string& url, std::string& text, std::string& error, size_t limit) {
    text.clear();
    if (!IsHttp(url)) {
        std::ifstream file(fs::u8path(url), std::ios::binary);
        if (!file) {
            error = "cannot read " + url;
            return false;
        }
        std::ostringstream out;
        out << file.rdbuf();
        text = out.str();
        if (text.size() > limit) {
            error = "too_large";
            return false;
        }
        return true;
    }
#if defined(_WIN32)
    uint64_t total = 0;
    return HttpGet(url, [&](const char* data, size_t size) {
        if (text.size() + size > limit) return false;
        text.append(data, size);
        return true;
    }, total, error);
#else
    error = "network downloads are only available on Windows";
    return false;
#endif
}

bool Download(const std::string& url, const fs::path& destination, uint64_t limit, std::string& error,
              const Progress& progress) {
    std::error_code ec;
    fs::create_directories(destination.parent_path(), ec);
    fs::path partial = destination;
    partial += ".part";
    if (!IsHttp(url)) {
        fs::copy_file(fs::u8path(url), partial, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            error = "cannot copy " + url + ": " + ec.message();
            return false;
        }
    } else {
#if defined(_WIN32)
        std::ofstream out(partial, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "cannot write " + partial.u8string();
            return false;
        }
        uint64_t written = 0;
        uint64_t total = 0;
        const bool ok = HttpGet(url, [&](const char* data, size_t size) {
            if (written + size > limit) return false;
            out.write(data, static_cast<std::streamsize>(size));
            written += size;
            if (progress) progress(written, total);
            return static_cast<bool>(out);
        }, total, error);
        out.close();
        if (!ok) {
            fs::remove(partial, ec);
            return false;
        }
#else
        error = "network downloads are only available on Windows";
        return false;
#endif
    }
    fs::rename(partial, destination, ec);
    if (ec) {
        fs::remove(destination, ec);
        fs::rename(partial, destination, ec);
    }
    if (ec) {
        error = "cannot finish " + destination.u8string() + ": " + ec.message();
        return false;
    }
    return true;
}

UpdateCheck CheckForUpdate(const std::string& feed, const std::string& currentVersion) {
    UpdateCheck check;
    check.current = currentVersion;
    std::string text;
    if (!FetchText(feed.empty() ? kDefaultUpdateFeed : feed, text, check.error)) return check;
    const json document = json::parse(text, nullptr, false);
    if (document.is_discarded()) {
        check.error = "the feed is not JSON";
        return check;
    }
    if (!ParseRelease(document, check.latest, check.error)) return check;
    check.ok = true;
    check.available = CompareVersions(check.latest.version, currentVersion) > 0;
    return check;
}

PreparedUpdate PrepareUpdate(const Release& release, InstallKind kind, const fs::path& workDirectory,
                             const Progress& progress) {
    PreparedUpdate update;
    update.kind = kind;
    const ReleaseAsset* asset = release.Find(AssetSuffix(kind));
    if (!asset) {
        update.error = std::string("the release has no ") + (kind == InstallKind::Installed ? "installer" : "portable archive");
        return update;
    }
    const ReleaseAsset* sums = release.Find("SHA256SUMS.txt");
    if (!sums) {
        update.error = "the release publishes no SHA256SUMS.txt; the download cannot be verified";
        return update;
    }
    std::string text;
    if (!FetchText(sums->url, text, update.error, 64 * 1024)) return update;
    const std::string expected = ChecksumFor(text, asset->name);
    if (expected.empty()) {
        update.error = "SHA256SUMS.txt has no entry for " + asset->name;
        return update;
    }
    const fs::path file = workDirectory / asset->name;
    if (!Download(asset->url, file, 512ull << 20, update.error, progress)) return update;
    const std::string actual = FileSha256(file);
    if (actual != expected) {
        std::error_code ec;
        fs::remove(file, ec);
        update.error = "the download does not match its published SHA-256 (expected " + expected + ", got " +
                       (actual.empty() ? std::string("unreadable") : actual) + "); it was deleted";
        return update;
    }
    update.ok = true;
    update.file = file;
    update.sha256 = actual;
    return update;
}

std::vector<BlockingProcess> FindBlockingProcesses(const fs::path& applicationDirectory) {
    std::vector<BlockingProcess> blocking;
#if defined(_WIN32)
    std::error_code ec;
    const std::wstring root = Wide(Lower(fs::weakly_canonical(applicationDirectory, ec).u8string()));
    auto under = [&root](const std::wstring& path) {
        std::wstring lower = path;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
        return !root.empty() && lower.rfind(root, 0) == 0;
    };
    const DWORD self = GetCurrentProcessId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return blocking;
    PROCESSENTRY32W process{};
    process.dwSize = sizeof(process);
    for (BOOL more = Process32FirstW(snapshot, &process); more; more = Process32NextW(snapshot, &process)) {
        if (process.th32ProcessID == self || process.th32ProcessID == 0) continue;
        HANDLE modules = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, process.th32ProcessID);
        if (modules == INVALID_HANDLE_VALUE) continue;
        MODULEENTRY32W module{};
        module.dwSize = sizeof(module);
        std::string reason;
        for (BOOL m = Module32FirstW(modules, &module); m; m = Module32NextW(modules, &module)) {
            if (!under(module.szExePath)) continue;
            const std::string name = Lower(Narrow(module.szModule));
            if (name == "cortex.exe") reason = "Cortex is running from this folder";
            else if (name == "cortex_core.dll") reason = "the Cortex runtime is loaded in it";
            else if (reason.empty()) reason = "it uses a file of this folder";
        }
        CloseHandle(modules);
        if (!reason.empty())
            blocking.push_back({process.th32ProcessID, Narrow(process.szExeFile), reason});
    }
    CloseHandle(snapshot);
#else
    (void)applicationDirectory;
#endif
    return blocking;
}

bool ExtractZip(const fs::path& archive, const fs::path& destination, std::string& error) {
    auto source = artifacts::OpenFile(archive.u8string(), &error);
    if (!source) return false;
    artifacts::ZipProvider zip;
    artifacts::InspectOptions all;
    all.maxEntries = 100000;
    const auto children = zip.Children(*source, all, error);
    if (!error.empty()) return false;
    std::error_code ec;
    fs::create_directories(destination, ec);
    const fs::path root = fs::weakly_canonical(destination, ec);
    for (const auto& child : children) {
        const fs::path relative = fs::u8path(child.name).lexically_normal();
        if (relative.empty() || relative.is_absolute() || relative.has_root_name() ||
            relative.u8string().rfind("..", 0) == 0) {
            error = "unsafe entry name in the archive: " + child.name;
            return false;
        }
        const fs::path target = (root / relative).lexically_normal();
        if (target.u8string().rfind(root.u8string(), 0) != 0) {
            error = "unsafe entry name in the archive: " + child.name;
            return false;
        }
        if (!child.readable) {
            error = "unreadable entry in the archive: " + child.name;
            return false;
        }
        std::string childError;
        const auto bytes = zip.OpenChild(source, child, childError);
        if (!bytes) {
            error = "cannot unpack " + child.name + ": " + childError;
            return false;
        }
        fs::create_directories(target.parent_path(), ec);
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        const uint64_t size = bytes->Size();
        std::vector<char> buffer(1 << 16);
        for (uint64_t offset = 0; offset < size && out;) {
            const size_t chunk = static_cast<size_t>(std::min<uint64_t>(buffer.size(), size - offset));
            if (!bytes->Read(offset, buffer.data(), chunk)) break;
            out.write(buffer.data(), static_cast<std::streamsize>(chunk));
            offset += chunk;
        }
        if (!out) {
            error = "cannot write " + target.u8string();
            return false;
        }
    }
    return true;
}

ApplyResult ApplyUpdate(const PreparedUpdate& update, const fs::path& applicationDirectory, const std::string& version) {
    ApplyResult result;
    if (!update.ok) {
        result.error = "the update was not verified";
        return result;
    }
    if (update.kind == InstallKind::Installed) {
#if defined(_WIN32)
        // The installer closes the running Cortex itself (AppMutex) and
        // reopens it when done.
        const std::wstring file = update.file.wstring();
        const HINSTANCE started = ShellExecuteW(nullptr, L"open", file.c_str(), L"/SILENT /SUPPRESSMSGBOXES /NORESTART /RELAUNCH=1",
                                                nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(started) <= 32) {
            result.error = "the installer did not start (" + std::to_string(reinterpret_cast<INT_PTR>(started)) + ")";
            return result;
        }
        result.ok = true;
        result.exitToFinish = true;
#else
        result.error = "installers run on Windows only";
#endif
        return result;
    }
    // Portable: a new folder beside the current one; the old one stays.
    const fs::path parent = applicationDirectory.parent_path();
    fs::path folder = parent / ("cortex-v" + version);
    std::error_code ec;
    for (int i = 2; fs::exists(folder, ec) && i < 100; ++i) folder = parent / ("cortex-v" + version + "-" + std::to_string(i));
    std::string error;
    if (!ExtractZip(update.file, folder, error)) {
        result.error = error;
        return result;
    }
    // An archive with one top folder: the program is inside it.
    if (!fs::exists(folder / "cortex.exe", ec)) {
        for (const auto& entry : fs::directory_iterator(folder, ec))
            if (entry.is_directory(ec) && fs::exists(entry.path() / "cortex.exe", ec)) folder = entry.path();
    }
    if (!fs::exists(folder / "cortex.exe", ec)) {
        result.error = "the archive has no cortex.exe";
        return result;
    }
    // Keep the person's choice of portable settings.
    if (fs::exists(applicationDirectory / "cortex.portable", ec)) {
        std::ofstream marker(folder / "cortex.portable");
    }
    result.ok = true;
    result.newFolder = folder;
    return result;
}

}  // namespace cortex::setup
