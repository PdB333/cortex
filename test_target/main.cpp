// cortex_test_target: minimal x86/x64 target for testing Cortex end-to-end.
//
// Exposes well-known memory values at exported symbols so tests can verify
// /memory/read, scans, freezes, crash capture and hangs without a real game.

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

extern "C" __declspec(dllexport) uint32_t g_cortex_u32   = 0xDEADBEEFu;
extern "C" __declspec(dllexport) uint64_t g_cortex_u64   = 0x0123456789ABCDEFull;
extern "C" __declspec(dllexport) float    g_cortex_float = 3.14159265f;
extern "C" __declspec(dllexport) double   g_cortex_double = 2.7182818284;
extern "C" __declspec(dllexport) char     g_cortex_str[32] = "cortex-canary";

extern "C" __declspec(dllexport) volatile uint32_t g_cortex_frame  = 0;
extern "C" __declspec(dllexport) volatile uint32_t g_cortex_wpress = 0;
extern "C" __declspec(dllexport) volatile uint32_t g_cortex_health = 100;
extern "C" __declspec(dllexport) volatile uint32_t g_cortex_breakpoint_canary = 0;
extern "C" __declspec(dllexport) volatile uint32_t g_cortex_hw_value = 0;
extern "C" __declspec(dllexport) volatile DWORD g_cortex_hw_writer_tid = 0;
extern "C" __declspec(dllexport) volatile uintptr_t g_cortex_step_over_sink = 0;

extern "C" __declspec(dllexport) __declspec(noinline) uintptr_t __cdecl CortexStepOverCallee(uintptr_t value) {
    return value ^ static_cast<uintptr_t>(0x5A5A);
}
extern "C" __declspec(dllexport) __declspec(noinline) uintptr_t __cdecl CortexStepOverCaller(uintptr_t value) {
    const uintptr_t result = CortexStepOverCallee(value);
    g_cortex_step_over_sink = result;
    return result + 1;
}

extern "C" __declspec(dllexport) __declspec(noinline) void CortexBreakpointCanary() {
    ++g_cortex_breakpoint_canary;
}
extern "C" __declspec(dllexport) __declspec(noinline) uintptr_t __cdecl CortexNativeAdd(uintptr_t a, uintptr_t b, uintptr_t c) {
    return a + b + c;
}
extern "C" __declspec(dllexport) __declspec(noinline) uintptr_t __cdecl CortexNativeIdentity(uintptr_t value) {
    return value;
}
extern "C" __declspec(dllexport) __declspec(noinline) uintptr_t __cdecl CortexNativeIncrementHwValue() {
    return static_cast<uintptr_t>(++g_cortex_hw_value);
}

struct CortexFakeObject {
    uintptr_t primaryVtable = 0;
    uintptr_t secondaryVtable = 0;
    uint32_t state = 7;
    uint32_t mode = 2;
};

extern "C" __declspec(dllexport) uintptr_t g_cortex_fake_vtable_primary[4]{};
extern "C" __declspec(dllexport) uintptr_t g_cortex_fake_vtable_secondary[4]{};
extern "C" __declspec(dllexport) CortexFakeObject g_cortex_fake_object{};

void InitializeFakeObject() {
    const uintptr_t add = reinterpret_cast<uintptr_t>(&CortexNativeAdd);
    const uintptr_t identity = reinterpret_cast<uintptr_t>(&CortexNativeIdentity);
    g_cortex_fake_vtable_primary[0] = add;
    g_cortex_fake_vtable_primary[1] = identity;
    g_cortex_fake_vtable_primary[2] = add;
    g_cortex_fake_vtable_primary[3] = identity;
    g_cortex_fake_vtable_secondary[0] = identity;
    g_cortex_fake_vtable_secondary[1] = add;
    g_cortex_fake_vtable_secondary[2] = identity;
    g_cortex_fake_vtable_secondary[3] = add;
    g_cortex_fake_object.primaryVtable = reinterpret_cast<uintptr_t>(&g_cortex_fake_vtable_primary[0]);
    g_cortex_fake_object.secondaryVtable = reinterpret_cast<uintptr_t>(&g_cortex_fake_vtable_secondary[0]);
}

DWORD WINAPI CortexHwWriterThread(void*) {
    g_cortex_hw_writer_tid = GetCurrentThreadId();
    Sleep(180);
    for (int i = 0; i < 8; ++i) {
        ++g_cortex_hw_value;
        Sleep(25);
    }
    return 0;
}

namespace {

struct E2EControl {
    bool enabled = false;
    std::string manifestPath;
    std::string crashEventName;
    std::string hangEventName;
    std::string stopEventName;
    std::string hwThreadEventName;
    HANDLE crashEvent = nullptr;
    HANDLE hangEvent = nullptr;
    HANDLE stopEvent = nullptr;
    HANDLE hwThreadEvent = nullptr;
};

std::vector<std::string> CommandLineArguments() {
    std::vector<std::string> arguments;
    std::string current;
    bool quoted = false;
    const char* commandLine = GetCommandLineA();
    for (const char* cursor = commandLine; cursor && *cursor; ++cursor) {
        if (*cursor == '"') {
            quoted = !quoted;
        } else if ((*cursor == ' ' || *cursor == '\t') && !quoted) {
            if (!current.empty()) {
                arguments.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(*cursor);
        }
    }
    if (!current.empty()) arguments.push_back(current);
    return arguments;
}

bool HasArgument(const std::vector<std::string>& arguments, const char* value) {
    for (const auto& argument : arguments)
        if (argument == value) return true;
    return false;
}

std::string OptionValue(const std::vector<std::string>& arguments, const char* option) {
    const std::string prefix = std::string(option) + "=";
    for (size_t index = 0; index < arguments.size(); ++index) {
        if (arguments[index].rfind(prefix, 0) == 0)
            return arguments[index].substr(prefix.size());
        if (arguments[index] == option && index + 1 < arguments.size())
            return arguments[index + 1];
    }
    return {};
}

std::string EscapeJson(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (unsigned char character : value) {
        switch (character) {
            case '\\': escaped += "\\\\"; break;
            case '"': escaped += "\\\""; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default: escaped.push_back(static_cast<char>(character)); break;
        }
    }
    return escaped;
}

void PrintCanary() {
    std::printf("cortex_test_target ready pid=%lu\n"
                "  &g_cortex_u32    = %p  = 0x%08X\n"
                "  &g_cortex_u64    = %p  = 0x%016llX\n"
                "  &g_cortex_float  = %p  = %f\n"
                "  &g_cortex_double = %p  = %f\n"
                "  &g_cortex_str    = %p  = \"%s\"\n"
                "  &g_cortex_frame  = %p\n"
                "  &g_cortex_health = %p\n",
                GetCurrentProcessId(),
                static_cast<void*>(&g_cortex_u32), g_cortex_u32,
                static_cast<void*>(&g_cortex_u64), static_cast<unsigned long long>(g_cortex_u64),
                static_cast<void*>(&g_cortex_float), g_cortex_float,
                static_cast<void*>(&g_cortex_double), g_cortex_double,
                static_cast<void*>(&g_cortex_str), g_cortex_str,
                const_cast<uint32_t*>(&g_cortex_frame),
                const_cast<uint32_t*>(&g_cortex_health));
    std::fflush(stdout);
}

__declspec(noinline) void TriggerNullCrash() {
    std::puts("cortex_test_target: triggering intentional null write");
    std::fflush(stdout);
    volatile uint32_t* pointer = nullptr;
    *pointer = 0xC07ECAFEu;
}

std::string EventName(const char* kind) {
    char name[96]{};
    std::snprintf(name, sizeof(name), "Local\\CortexE2E_%s_%lu", kind,
                  static_cast<unsigned long>(GetCurrentProcessId()));
    return name;
}

bool InitializeE2E(const std::vector<std::string>& arguments, E2EControl& control) {
    control.manifestPath = OptionValue(arguments, "--e2e-manifest");
    if (control.manifestPath.empty()) return true;

    control.enabled = true;
    control.crashEventName = EventName("Crash");
    control.hangEventName = EventName("Hang");
    control.stopEventName = EventName("Stop");
    control.hwThreadEventName = EventName("HwThread");
    control.crashEvent = CreateEventA(nullptr, TRUE, FALSE, control.crashEventName.c_str());
    control.hangEvent = CreateEventA(nullptr, TRUE, FALSE, control.hangEventName.c_str());
    control.stopEvent = CreateEventA(nullptr, TRUE, FALSE, control.stopEventName.c_str());
    control.hwThreadEvent = CreateEventA(nullptr, TRUE, FALSE, control.hwThreadEventName.c_str());
    return control.crashEvent && control.hangEvent && control.stopEvent && control.hwThreadEvent;
}

void WriteManifest(const E2EControl& control, HWND window) {
    if (!control.enabled) return;
    char modulePath[MAX_PATH]{};
    GetModuleFileNameA(nullptr, modulePath, MAX_PATH);

    std::ofstream file(control.manifestPath, std::ios::binary | std::ios::trunc);
    file << "{\n"
         << "  \"schema_version\": 1,\n"
         << "  \"pid\": " << GetCurrentProcessId() << ",\n"
         << "  \"pointer_size\": " << sizeof(void*) << ",\n"
         << "  \"module_path\": \"" << EscapeJson(modulePath) << "\",\n"
         << "  \"window\": \"0x" << std::hex << reinterpret_cast<uintptr_t>(window) << "\",\n"
         << "  \"u32\": \"0x" << reinterpret_cast<uintptr_t>(&g_cortex_u32) << "\",\n"
         << "  \"u64\": \"0x" << reinterpret_cast<uintptr_t>(&g_cortex_u64) << "\",\n"
         << "  \"float\": \"0x" << reinterpret_cast<uintptr_t>(&g_cortex_float) << "\",\n"
         << "  \"double\": \"0x" << reinterpret_cast<uintptr_t>(&g_cortex_double) << "\",\n"
         << "  \"string\": \"0x" << reinterpret_cast<uintptr_t>(&g_cortex_str) << "\",\n"
         << "  \"frame\": \"0x" << reinterpret_cast<uintptr_t>(&g_cortex_frame) << "\",\n"
         << "  \"health\": \"0x" << reinterpret_cast<uintptr_t>(&g_cortex_health) << "\",\n"
         << "  \"anchor\": \"0x" << reinterpret_cast<uintptr_t>(&TriggerNullCrash) << "\",\n"
         << "  \"breakpoint_anchor\": \"0x" << reinterpret_cast<uintptr_t>(&CortexBreakpointCanary) << "\",\n"
         << "  \"native_add\": \"0x" << reinterpret_cast<uintptr_t>(&CortexNativeAdd) << "\",\n"
         << "  \"step_over_function\": \"0x" << reinterpret_cast<uintptr_t>(&CortexStepOverCaller) << "\",\n"
         << "  \"native_increment_hw_value\": \"0x" << reinterpret_cast<uintptr_t>(&CortexNativeIncrementHwValue) << "\",\n"
         << "  \"hw_value\": \"0x" << reinterpret_cast<uintptr_t>(&g_cortex_hw_value) << "\",\n"
         << "  \"hw_writer_tid\": \"0x" << reinterpret_cast<uintptr_t>(&g_cortex_hw_writer_tid) << "\",\n"
         << "  \"fake_object\": \"0x" << reinterpret_cast<uintptr_t>(&g_cortex_fake_object) << "\",\n"
         << "  \"fake_object_size\": " << std::dec << sizeof(g_cortex_fake_object) << ",\n"
         << "  \"fake_state\": \"0x" << std::hex << reinterpret_cast<uintptr_t>(&g_cortex_fake_object.state) << "\",\n"
         << std::dec
         << "  \"main_thread_id\": " << GetCurrentThreadId() << ",\n"
         << "  \"crash_event\": \"" << EscapeJson(control.crashEventName) << "\",\n"
         << "  \"hang_event\": \"" << EscapeJson(control.hangEventName) << "\",\n"
         << "  \"stop_event\": \"" << EscapeJson(control.stopEventName) << "\",\n"
         << "  \"hw_thread_event\": \"" << EscapeJson(control.hwThreadEventName) << "\"\n"
         << "}\n";
    file.flush();
}

void CloseE2E(E2EControl& control) {
    if (control.crashEvent) CloseHandle(control.crashEvent);
    if (control.hangEvent) CloseHandle(control.hangEvent);
    if (control.stopEvent) CloseHandle(control.stopEvent);
    if (control.hwThreadEvent) CloseHandle(control.hwThreadEvent);
    control = {};
}

LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            RECT rectangle{};
            GetClientRect(window, &rectangle);
            HBRUSH background = CreateSolidBrush(RGB(0x33, 0x66, 0x99));
            FillRect(dc, &rectangle, background);
            DeleteObject(background);
            char text[128]{};
            std::snprintf(text, sizeof(text),
                          "cortex_test_target  frame=%u  W=%u  health=%u",
                          g_cortex_frame, g_cortex_wpress, g_cortex_health);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(255, 255, 255));
            TextOutA(dc, 10, 10, text, static_cast<int>(std::strlen(text)));
            EndPaint(window, &paint);
            return 0;
        }
    }
    return DefWindowProcA(window, message, wparam, lparam);
}

} // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int show) {
    PrintCanary();
    const auto arguments = CommandLineArguments();

    WNDCLASSA windowClass{};
    windowClass.lpfnWndProc = WndProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    windowClass.lpszClassName = "CortexTestTarget";
    RegisterClassA(&windowClass);

    HWND window = CreateWindowA(
        "CortexTestTarget", "cortex_test_target", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 640, 200,
        nullptr, nullptr, instance, nullptr);
    ShowWindow(window, show);

    InitializeFakeObject();
    E2EControl e2e;
    if (!InitializeE2E(arguments, e2e)) return 3;
    WriteManifest(e2e, window);

    const bool crashNull = HasArgument(arguments, "--crash-null");
    uint32_t crashCountdown = crashNull ? 60 : 0;

    for (;;) {
        MSG message{};
        while (PeekMessageA(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                CloseE2E(e2e);
                return 0;
            }
            TranslateMessage(&message);
            DispatchMessageA(&message);
        }

        if (e2e.enabled && WaitForSingleObject(e2e.stopEvent, 0) == WAIT_OBJECT_0) {
            CloseE2E(e2e);
            return 0;
        }
        if (e2e.enabled && WaitForSingleObject(e2e.crashEvent, 0) == WAIT_OBJECT_0)
            TriggerNullCrash();
        if (e2e.enabled && WaitForSingleObject(e2e.hwThreadEvent, 0) == WAIT_OBJECT_0) {
            ResetEvent(e2e.hwThreadEvent);
            HANDLE writer = CreateThread(nullptr, 0, CortexHwWriterThread, nullptr, 0, nullptr);
            if (writer) CloseHandle(writer);
        }
        if (e2e.enabled && WaitForSingleObject(e2e.hangEvent, 0) == WAIT_OBJECT_0) {
            while (WaitForSingleObject(e2e.stopEvent, 100) == WAIT_TIMEOUT) {}
            CloseE2E(e2e);
            return 0;
        }

        ++g_cortex_frame;
        CortexBreakpointCanary();
        g_cortex_step_over_sink = CortexStepOverCaller(g_cortex_frame);
        if (GetAsyncKeyState('W') & 0x8000) ++g_cortex_wpress;
        if (e2e.enabled && (g_cortex_frame % 15) == 0) ++g_cortex_health;
        InvalidateRect(window, nullptr, FALSE);
        if (crashCountdown > 0 && --crashCountdown == 0) TriggerNullCrash();
        Sleep(16);
    }
}




