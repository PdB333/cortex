#include "overlay.h"
#ifdef CORTEX_D3D8
#include "overlay_common.h"

namespace overlay {

void Init(IDirect3DDevice8*, HWND hwnd) {
    detail::CommonInit(hwnd);
}

void OnFrame(IDirect3DDevice8*) {
    detail::CommonFrame();
}

} // namespace overlay
#endif // CORTEX_D3D8
