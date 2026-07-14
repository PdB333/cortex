#include "imgui_impl_dx8.h"
#include <vector>
#include <cstring>
#include <cstdint>

namespace {

struct CustomVertex {
    float pos[4]; // x, y, z, rhw (pre-transformed, screen space)
    D3DCOLOR col;
    float uv[2];
};
#define D3DFVF_CUSTOMVERTEX (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)

IDirect3DDevice8* g_pd3dDevice = nullptr;
IDirect3DVertexBuffer8* g_pVB = nullptr;
IDirect3DIndexBuffer8* g_pIB = nullptr;
IDirect3DTexture8* g_FontTexture = nullptr;
int g_VertexBufferSize = 5000;
int g_IndexBufferSize = 10000;

void SetupRenderState(ImDrawData* draw_data) {
    D3DVIEWPORT8 vp;
    vp.X = 0; vp.Y = 0;
    vp.Width = static_cast<DWORD>(draw_data->DisplaySize.x);
    vp.Height = static_cast<DWORD>(draw_data->DisplaySize.y);
    vp.MinZ = 0.0f; vp.MaxZ = 1.0f;
    g_pd3dDevice->SetViewport(&vp);

    g_pd3dDevice->SetVertexShader(D3DFVF_CUSTOMVERTEX);
    g_pd3dDevice->SetPixelShader(0);

    g_pd3dDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    g_pd3dDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_pd3dDevice->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    g_pd3dDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    g_pd3dDevice->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    g_pd3dDevice->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    g_pd3dDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    g_pd3dDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    g_pd3dDevice->SetRenderState(D3DRS_FOGENABLE, FALSE);
    g_pd3dDevice->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    g_pd3dDevice->SetRenderState(D3DRS_CLIPPING, TRUE);
    g_pd3dDevice->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);

    g_pd3dDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    g_pd3dDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    g_pd3dDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    g_pd3dDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    g_pd3dDevice->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    g_pd3dDevice->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    g_pd3dDevice->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
    g_pd3dDevice->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
    g_pd3dDevice->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    g_pd3dDevice->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    g_pd3dDevice->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
}

// D3D8 has no hardware scissor test; approximate per-draw-command clipping by
// shrinking the viewport to the command's clip rect (fine for axis-aligned,
// non-rotated 2D UI, which is all ImGui ever produces).
// D3D8 has no scene-local state stack, so every render state / texture stage
// state / stream source we touch to draw the UI otherwise leaks straight into
// the game's own next-frame rendering (nothing resets it between our
// EndScene hook and the game's next BeginScene) -- this is what caused the
// visual corruption when opening the overlay. Snapshot the device's current
// state on construction, reapply it on destruction, regardless of how the
// scope exits.
struct StateBlockGuard {
    IDirect3DDevice8* device;
    DWORD token = 0;
    explicit StateBlockGuard(IDirect3DDevice8* dev) : device(dev) {
        if (device->CreateStateBlock(D3DSBT_ALL, &token) < 0) token = 0;
    }
    ~StateBlockGuard() {
        if (token) {
            device->ApplyStateBlock(token);
            device->DeleteStateBlock(token);
        }
    }
};

void ApplyClipViewport(const ImVec4& clip, const ImVec2& displaySize) {
    LONG x = static_cast<LONG>(clip.x < 0 ? 0 : clip.x);
    LONG y = static_cast<LONG>(clip.y < 0 ? 0 : clip.y);
    LONG right = static_cast<LONG>(clip.z > displaySize.x ? displaySize.x : clip.z);
    LONG bottom = static_cast<LONG>(clip.w > displaySize.y ? displaySize.y : clip.w);
    if (right <= x || bottom <= y) return;

    D3DVIEWPORT8 vp;
    vp.X = static_cast<DWORD>(x);
    vp.Y = static_cast<DWORD>(y);
    vp.Width = static_cast<DWORD>(right - x);
    vp.Height = static_cast<DWORD>(bottom - y);
    vp.MinZ = 0.0f; vp.MaxZ = 1.0f;
    g_pd3dDevice->SetViewport(&vp);
}

} // namespace

bool ImGui_ImplDX8_CreateDeviceObjects() {
    if (!g_pd3dDevice) return false;

    if (g_pd3dDevice->CreateVertexBuffer(
            g_VertexBufferSize * sizeof(CustomVertex),
            D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFVF_CUSTOMVERTEX,
            D3DPOOL_DEFAULT, &g_pVB) < 0) return false;

    if (g_pd3dDevice->CreateIndexBuffer(
            g_IndexBufferSize * sizeof(ImDrawIdx),
            D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
            sizeof(ImDrawIdx) == 2 ? D3DFMT_INDEX16 : D3DFMT_INDEX32,
            D3DPOOL_DEFAULT, &g_pIB) < 0) return false;

    ImGuiIO& io = ImGui::GetIO();
    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

    if (g_pd3dDevice->CreateTexture(width, height, 1, 0, D3DFMT_A8R8G8B8,
                                     D3DPOOL_MANAGED, &g_FontTexture) < 0) return false;

    D3DLOCKED_RECT lockedRect;
    if (g_FontTexture->LockRect(0, &lockedRect, nullptr, 0) < 0) return false;
    for (int y = 0; y < height; ++y) {
        uint32_t* dst = reinterpret_cast<uint32_t*>(
            reinterpret_cast<uint8_t*>(lockedRect.pBits) + y * lockedRect.Pitch);
        const uint32_t* src = reinterpret_cast<const uint32_t*>(pixels) + y * width;
        for (int x = 0; x < width; ++x) {
            uint32_t p = src[x]; // RGBA
            uint8_t r = (p >> 0) & 0xFF, g = (p >> 8) & 0xFF, b = (p >> 16) & 0xFF, a = (p >> 24) & 0xFF;
            dst[x] = (a << 24) | (r << 16) | (g << 8) | b; // -> ARGB
        }
    }
    g_FontTexture->UnlockRect(0);

    io.Fonts->SetTexID(reinterpret_cast<ImTextureID>(g_FontTexture));
    return true;
}

void ImGui_ImplDX8_InvalidateDeviceObjects() {
    if (g_pVB) { g_pVB->Release(); g_pVB = nullptr; }
    if (g_pIB) { g_pIB->Release(); g_pIB = nullptr; }
    if (g_FontTexture) {
        g_FontTexture->Release();
        g_FontTexture = nullptr;
        ImGui::GetIO().Fonts->SetTexID(0);
    }
}

bool ImGui_ImplDX8_Init(IDirect3DDevice8* device) {
    g_pd3dDevice = device;
    return true;
}

void ImGui_ImplDX8_Shutdown() {
    ImGui_ImplDX8_InvalidateDeviceObjects();
    g_pd3dDevice = nullptr;
}

void ImGui_ImplDX8_NewFrame() {
    if (!g_FontTexture) ImGui_ImplDX8_CreateDeviceObjects();
}

void ImGui_ImplDX8_RenderDrawData(ImDrawData* draw_data) {
    if (!g_pd3dDevice || draw_data->DisplaySize.x <= 0 || draw_data->DisplaySize.y <= 0) return;
    if (draw_data->CmdListsCount == 0) return;
    if (!g_pVB && !ImGui_ImplDX8_CreateDeviceObjects()) return;

    StateBlockGuard stateGuard(g_pd3dDevice);

    // Grow buffers if needed.
    if (draw_data->TotalVtxCount > g_VertexBufferSize) {
        if (g_pVB) { g_pVB->Release(); g_pVB = nullptr; }
        g_VertexBufferSize = draw_data->TotalVtxCount + 5000;
        if (g_pd3dDevice->CreateVertexBuffer(
                g_VertexBufferSize * sizeof(CustomVertex),
                D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFVF_CUSTOMVERTEX,
                D3DPOOL_DEFAULT, &g_pVB) < 0) return;
    }
    if (draw_data->TotalIdxCount > g_IndexBufferSize) {
        if (g_pIB) { g_pIB->Release(); g_pIB = nullptr; }
        g_IndexBufferSize = draw_data->TotalIdxCount + 10000;
        if (g_pd3dDevice->CreateIndexBuffer(
                g_IndexBufferSize * sizeof(ImDrawIdx),
                D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
                sizeof(ImDrawIdx) == 2 ? D3DFMT_INDEX16 : D3DFMT_INDEX32,
                D3DPOOL_DEFAULT, &g_pIB) < 0) return;
    }

    // Upload vertex/index data.
    CustomVertex* vtxDst;
    if (g_pVB->Lock(0, 0, reinterpret_cast<BYTE**>(&vtxDst), D3DLOCK_DISCARD) < 0) return;
    ImDrawIdx* idxDst;
    if (g_pIB->Lock(0, 0, reinterpret_cast<BYTE**>(&idxDst), D3DLOCK_DISCARD) < 0) {
        g_pVB->Unlock();
        return;
    }

    // D3D8's DrawIndexedPrimitive has neither a BaseVertexIndex parameter nor
    // a vertex-buffer stream offset (both added later in D3D9), so unlike
    // the D3D9/D3D11 ImGui backends we can't rely on the GPU to add a
    // per-draw-list vertex offset: indices must be made absolute into the
    // single shared vertex buffer ourselves before upload.
    int vtxOffsetAccum = 0;
    for (int n = 0; n < draw_data->CmdListsCount; ++n) {
        const ImDrawList* cmdList = draw_data->CmdLists[n];
        const ImDrawVert* vtxSrc = cmdList->VtxBuffer.Data;
        for (int i = 0; i < cmdList->VtxBuffer.Size; ++i) {
            vtxDst->pos[0] = vtxSrc->pos.x;
            vtxDst->pos[1] = vtxSrc->pos.y;
            vtxDst->pos[2] = 0.0f;
            vtxDst->pos[3] = 1.0f;
            ImU32 c = vtxSrc->col; // ImGui gives RGBA
            uint8_t r = (c >> 0) & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF, a = (c >> 24) & 0xFF;
            vtxDst->col = D3DCOLOR_ARGB(a, r, g, b);
            vtxDst->uv[0] = vtxSrc->uv.x;
            vtxDst->uv[1] = vtxSrc->uv.y;
            ++vtxDst; ++vtxSrc;
        }
        const ImDrawIdx* idxSrc = cmdList->IdxBuffer.Data;
        for (int i = 0; i < cmdList->IdxBuffer.Size; ++i) {
            idxDst[i] = static_cast<ImDrawIdx>(idxSrc[i] + vtxOffsetAccum);
        }
        idxDst += cmdList->IdxBuffer.Size;
        vtxOffsetAccum += cmdList->VtxBuffer.Size;
    }
    g_pVB->Unlock();
    g_pIB->Unlock();

    g_pd3dDevice->SetStreamSource(0, g_pVB, sizeof(CustomVertex));
    g_pd3dDevice->SetIndices(g_pIB, 0);

    SetupRenderState(draw_data);

    int globalVtxOffset = 0;
    int globalIdxOffset = 0;
    ImTextureID lastTexId = draw_data->CmdLists[0]->CmdBuffer.Size > 0
        ? draw_data->CmdLists[0]->CmdBuffer[0].TextureId : nullptr;
    g_pd3dDevice->SetTexture(0, reinterpret_cast<IDirect3DTexture8*>(lastTexId));

    for (int n = 0; n < draw_data->CmdListsCount; ++n) {
        const ImDrawList* cmdList = draw_data->CmdLists[n];
        for (int c = 0; c < cmdList->CmdBuffer.Size; ++c) {
            const ImDrawCmd& cmd = cmdList->CmdBuffer[c];
            if (cmd.UserCallback) {
                cmd.UserCallback(cmdList, &cmd);
                continue;
            }

            if (cmd.TextureId != lastTexId) {
                lastTexId = cmd.TextureId;
                g_pd3dDevice->SetTexture(0, reinterpret_cast<IDirect3DTexture8*>(lastTexId));
            }

            ApplyClipViewport(cmd.ClipRect, draw_data->DisplaySize);

            // D3D8's DrawIndexedPrimitive has no base-vertex parameter, so we
            // pass the vertex-buffer's global offset explicitly via minIndex
            // and rely on indices in the (small, per-draw-list) 16-bit range.
            g_pd3dDevice->DrawIndexedPrimitive(
                D3DPT_TRIANGLELIST,
                globalVtxOffset,
                static_cast<UINT>(cmdList->VtxBuffer.Size),
                globalIdxOffset + cmd.IdxOffset,
                cmd.ElemCount / 3);
        }
        globalVtxOffset += cmdList->VtxBuffer.Size;
        globalIdxOffset += cmdList->IdxBuffer.Size;
    }
    // Game's render state (viewport, render states, texture stages, stream
    // source, ...) is restored by stateGuard's destructor here.
}
