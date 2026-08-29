#include "../core/call/call.h"

#include <windows.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

static uintptr_t __cdecl Add3(uintptr_t a, uintptr_t b, uintptr_t c) {
    return a + b + c;
}

int main() {
    remotecall::Init();
    std::atomic<bool> running{true};
    std::atomic<DWORD> pumpTid{0};
    std::thread frame([&] {
        pumpTid.store(GetCurrentThreadId(), std::memory_order_release);
        while (running.load(std::memory_order_acquire)) {
            remotecall::PumpGameThread();
            Sleep(2);
        }
    });

    const ULONGLONG deadline = GetTickCount64() + 1000;
    while (remotecall::GameThreadId() == 0 && GetTickCount64() < deadline) Sleep(1);
    auto result = remotecall::InvokeOnGameThread(
        reinterpret_cast<uintptr_t>(&Add3), {10,20,12}, remotecall::Convention::Cdecl, 1000);
    int failures = 0;
    auto check = [&](bool condition, const char* message) {
        if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
    };
    check(result.ok, "queued game-thread call succeeds");
    check(result.returnValue == 42, "queued game-thread call returns value");
    check(result.threadId == pumpTid.load(), "call executes on the frame pump thread");

    running.store(false, std::memory_order_release);
    frame.join();
    Sleep(5);
    const auto started = GetTickCount64();
    auto timeout = remotecall::InvokeOnGameThread(
        reinterpret_cast<uintptr_t>(&Add3), {1,2,3}, remotecall::Convention::Cdecl, 40);
    const auto elapsed = GetTickCount64() - started;
    check(!timeout.ok && timeout.error.find("timeout") != std::string::npos,
          "missing frame produces a bounded timeout");
    check(elapsed < 500, "game-thread timeout is bounded");
    check(remotecall::Shutdown(), "scheduler shuts down cleanly");
    if (failures) return 1;
    std::cout << "PASS: game-thread native call scheduler\n";
    return 0;
}
