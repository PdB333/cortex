#pragma once
#include <string>

namespace hook {

// Installs a Present-family hook for whichever render API the process is
// actually using (D3D9/D3D10/D3D11/D3D12/OpenGL), auto-detected via kiero.
// Vulkan is intentionally not supported: kiero's stock method table only
// resolves core Vulkan 1.0 entry points via flat GetProcAddress, and the
// actual present/swapchain functions (vkQueuePresentKHR, vkCreateSwapchainKHR,
// etc.) are VK_KHR_swapchain extension functions only obtainable through
// vkGetInstanceProcAddr/vkGetDeviceProcAddr -- not something kiero as shipped
// can hook, so it is out of scope here.
bool InitKieroHook();
// Tries immediately, then retries in the background while the game loads its
// renderer. Useful for ASI loaders that inject Cortex before d3d*.dll exists.
bool StartKieroHookWithRetry();
void StopKieroHookRetry();
void ShutdownKieroHook();
bool IsKieroHookInstalled();
std::string GetKieroRenderBackend();

} // namespace hook
