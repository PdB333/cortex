#include "dinput_hook.h"
#include "../log.h"

#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <MinHook.h>
#include <atomic>
#include <mutex>

namespace hook {

namespace {
    std::atomic<bool> g_captureActive{false};

    typedef HRESULT(STDMETHODCALLTYPE* Acquire_t)(void*);
    typedef HRESULT(STDMETHODCALLTYPE* Unacquire_t)(void*);

    Acquire_t oAcquire = nullptr;
    Unacquire_t oUnacquire = nullptr;

    // The game may have created its DirectInput devices (keyboard, mouse,
    // ...) before we got injected, so we can never see its actual device
    // pointers via CreateDevice. Instead we track every device that
    // successfully Acquire()s through our hook -- covers the real game
    // devices regardless of when they were created, since the hook is on
    // the shared vtable function address, not a specific instance.
    constexpr int kMaxTrackedDevices = 8;
    std::mutex g_devicesMutex;
    void* g_acquiredDevices[kMaxTrackedDevices] = {};

    void TrackAcquired(void* dev) {
        std::lock_guard<std::mutex> lock(g_devicesMutex);
        for (auto& slot : g_acquiredDevices) if (slot == dev) return;
        for (auto& slot : g_acquiredDevices) if (!slot) { slot = dev; return; }
    }

    void UntrackAcquired(void* dev) {
        std::lock_guard<std::mutex> lock(g_devicesMutex);
        for (auto& slot : g_acquiredDevices) if (slot == dev) { slot = nullptr; return; }
    }

    HRESULT STDMETHODCALLTYPE hkAcquire(void* self) {
        if (g_captureActive.load(std::memory_order_relaxed)) {
            // Swallow: pretend success but don't actually (re-)acquire, so
            // exclusive HID capture never gets re-grabbed while the
            // overlay/popup owns the cursor.
            dbglog::Line("dinput: hkAcquire(%p) swallowed (captureActive)", self);
            return DI_OK;
        }
        HRESULT hr = oAcquire(self);
        dbglog::Line("dinput: hkAcquire(%p) passthrough hr=0x%08lX", self, (unsigned long)hr);
        if (SUCCEEDED(hr)) TrackAcquired(self);
        return hr;
    }

    HRESULT STDMETHODCALLTYPE hkUnacquire(void* self) {
        HRESULT hr = oUnacquire(self);
        dbglog::Line("dinput: hkUnacquire(%p) hr=0x%08lX", self, (unsigned long)hr);
        UntrackAcquired(self);
        return hr;
    }

    // Creates a throwaway mouse device solely to read Acquire/Unacquire
    // vtable addresses. Same trick as GetD3D8VTableAddresses: the vtable is
    // shared by every device instance from this DLL's DirectInput
    // implementation, so hooking these addresses also hooks the game's real
    // (already-created) devices.
    bool GetDInputDeviceVTableAddresses(void** outAcquire, void** outUnacquire) {
        HMODULE dinput8 = GetModuleHandleA("dinput8.dll");
        if (!dinput8) {
            dbglog::Line("dinput: dinput8.dll not loaded");
            return false;
        }
        auto pDirectInput8Create = reinterpret_cast<HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN)>(
            GetProcAddress(dinput8, "DirectInput8Create"));
        if (!pDirectInput8Create) {
            dbglog::Line("dinput: GetProcAddress(DirectInput8Create) failed");
            return false;
        }

        bool ok = false;
        void* di = nullptr;
        HRESULT hr = pDirectInput8Create(GetModuleHandleA(nullptr), DIRECTINPUT_VERSION,
                                          IID_IDirectInput8A, &di, nullptr);
        dbglog::Line("dinput: dummy DirectInput8Create hr=0x%08lX di=%p", (unsigned long)hr, di);
        if (SUCCEEDED(hr) && di) {
            void** diVtable = *reinterpret_cast<void***>(di);
            typedef HRESULT(STDMETHODCALLTYPE* CreateDevice_t)(void*, REFGUID, void**, LPUNKNOWN);
            auto createDevice = reinterpret_cast<CreateDevice_t>(diVtable[3]);

            void* device = nullptr;
            HRESULT hr2 = createDevice(di, GUID_SysMouse, &device, nullptr);
            dbglog::Line("dinput: dummy CreateDevice(mouse) hr=0x%08lX device=%p", (unsigned long)hr2, device);
            if (SUCCEEDED(hr2) && device) {
                void** devVtable = *reinterpret_cast<void***>(device);
                *outAcquire = devVtable[7];
                *outUnacquire = devVtable[8];
                dbglog::Line("dinput: device vtable=%p Acquire=%p Unacquire=%p", (void*)devVtable, *outAcquire, *outUnacquire);
                reinterpret_cast<IUnknown*>(device)->Release();
                ok = true;
            }
            reinterpret_cast<IUnknown*>(di)->Release();
        }
        return ok;
    }
}

bool InitDInputHook() {
    void* acquireAddr = nullptr;
    void* unacquireAddr = nullptr;
    if (!GetDInputDeviceVTableAddresses(&acquireAddr, &unacquireAddr)) {
        dbglog::Line("InitDInputHook: GetDInputDeviceVTableAddresses failed");
        return false;
    }

    MH_STATUS s1 = MH_CreateHook(acquireAddr, reinterpret_cast<void*>(&hkAcquire), reinterpret_cast<void**>(&oAcquire));
    MH_STATUS s2 = MH_CreateHook(unacquireAddr, reinterpret_cast<void*>(&hkUnacquire), reinterpret_cast<void**>(&oUnacquire));
    dbglog::Line("InitDInputHook create: Acquire=%d Unacquire=%d", (int)s1, (int)s2);
    if (s1 != MH_OK || s2 != MH_OK) return false;

    MH_STATUS e1 = MH_EnableHook(acquireAddr);
    MH_STATUS e2 = MH_EnableHook(unacquireAddr);
    dbglog::Line("InitDInputHook enable: Acquire=%d Unacquire=%d", (int)e1, (int)e2);
    return e1 == MH_OK && e2 == MH_OK;
}

void SetDInputCaptureActive(bool active) {
    bool was = g_captureActive.exchange(active, std::memory_order_relaxed);
    if (!was && active && oUnacquire) {
        // Entering capture: force-release every device we've seen acquired
        // so far (the game's real mouse/keyboard among them) right away,
        // instead of waiting for the game to notice on its own.
        std::lock_guard<std::mutex> lock(g_devicesMutex);
        int count = 0;
        for (auto& slot : g_acquiredDevices) {
            if (slot) {
                oUnacquire(slot);
                slot = nullptr;
                ++count;
            }
        }
        dbglog::Line("dinput: SetDInputCaptureActive(true) force-unacquired %d device(s)", count);
    } else if (was != active) {
        dbglog::Line("dinput: SetDInputCaptureActive(%d)", (int)active);
    }
}

} // namespace hook
