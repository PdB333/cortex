#pragma once
#include <cstdint>
#include <mutex>
#include <condition_variable>
#include <vector>

// Backend-agnostic capture state/plumbing shared by the kiero (D3D9-D3D12/
// OpenGL) and D3D8 backend implementations. Deliberately free of any D3D
// header so it can be included from both capture.cpp and capture_d3d8.cpp
// without triggering the d3d8.h/d3d9.h type-redefinition clash (MinGW's
// d3d8types.h and d3d9types.h define the same struct names and cannot
// coexist in one translation unit).
namespace capture::detail {

extern std::mutex g_mutex;
extern std::condition_variable g_cv;
extern bool g_requested;
extern bool g_ready;
extern std::vector<uint8_t> g_png;
// Rolling copy of the most recent PNG we ever produced (any backend). Kept
// so /screenshot?mode=last (and the auto fallback) can answer instantly
// when the render loop has stalled -- e.g. the game is minimized, alt-tabbed
// out of D3D8 exclusive fullscreen, or otherwise not currently Presenting.
extern std::vector<uint8_t> g_lastPng;
extern unsigned long long g_lastPngTickMs;

// Encodes tightly-packed RGBA8 pixels to PNG and stashes the result as the
// pending capture result. Must be called with g_mutex held. swapRB=false
// for backends (OpenGL) whose readback is already RGBA.
void FinishCapture(std::vector<uint8_t>& pixels, uint32_t width, uint32_t height, bool swapRB = true);

} // namespace capture::detail
