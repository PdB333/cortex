#pragma once
// Minimal Dear ImGui renderer backend for Direct3D 8. There is no official
// ImGui backend for D3D8 (it predates ImGui by a decade); this is a small
// custom one modeled on the official imgui_impl_dx9 backend, adapted for the
// D3D8 API differences (no hardware scissor test, FVF set through
// SetVertexShader, texture filtering via texture-stage states instead of
// sampler states, DrawIndexedPrimitive without a base-vertex parameter).
#include <d3d8.h>
#include <imgui.h>

bool ImGui_ImplDX8_Init(IDirect3DDevice8* device);
void ImGui_ImplDX8_Shutdown();
void ImGui_ImplDX8_NewFrame();
void ImGui_ImplDX8_RenderDrawData(ImDrawData* draw_data);

// Call around device->Reset().
void ImGui_ImplDX8_InvalidateDeviceObjects();
bool ImGui_ImplDX8_CreateDeviceObjects();
