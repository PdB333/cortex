#include "screenshot_workspace.h"

#include <imgui.h>

#include <d3d11.h>
#include <objbase.h>
#include <wincrypt.h>
#include <wincodec.h>
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace cortex::ui {
namespace {

constexpr const char* kModes[] = {"auto", "render", "window", "last"};

template <typename T>
void ReleaseCom(T*& value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

bool DecodeBase64(const std::string& input, std::vector<uint8_t>& output,
                  std::string& error) {
    output.clear();
    DWORD size = 0;
    if (!CryptStringToBinaryA(input.c_str(), static_cast<DWORD>(input.size()),
                              CRYPT_STRING_BASE64, nullptr, &size,
                              nullptr, nullptr) || size == 0) {
        error = "screenshot_base64_size_failed:" +
                std::to_string(GetLastError());
        return false;
    }
    output.resize(size);
    if (!CryptStringToBinaryA(input.c_str(), static_cast<DWORD>(input.size()),
                              CRYPT_STRING_BASE64, output.data(), &size,
                              nullptr, nullptr)) {
        error = "screenshot_base64_decode_failed:" +
                std::to_string(GetLastError());
        output.clear();
        return false;
    }
    output.resize(size);
    return true;
}

bool DecodePngRgba(const std::vector<uint8_t>& encoded,
                   std::vector<uint8_t>& pixels,
                   UINT& width, UINT& height,
                   std::string& error) {
    pixels.clear();
    width = height = 0;
    if (encoded.empty() ||
        encoded.size() > static_cast<size_t>(std::numeric_limits<DWORD>::max())) {
        error = "screenshot_image_size_invalid";
        return false;
    }

    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninitialize = SUCCEEDED(init);
    if (FAILED(init) && init != RPC_E_CHANGED_MODE) {
        error = "screenshot_com_init_failed";
        return false;
    }

    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;

    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) hr = factory->CreateStream(&stream);
    if (SUCCEEDED(hr)) {
        hr = stream->InitializeFromMemory(
            const_cast<BYTE*>(reinterpret_cast<const BYTE*>(encoded.data())),
            static_cast<DWORD>(encoded.size()));
    }
    if (SUCCEEDED(hr)) {
        hr = factory->CreateDecoderFromStream(
            stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
    }
    if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(hr)) hr = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr)) {
        hr = converter->Initialize(
            frame, GUID_WICPixelFormat32bppRGBA,
            WICBitmapDitherTypeNone, nullptr, 0.0,
            WICBitmapPaletteTypeCustom);
    }
    if (SUCCEEDED(hr)) hr = converter->GetSize(&width, &height);

    if (SUCCEEDED(hr) && width > 0 && height > 0) {
        const uint64_t stride64 = static_cast<uint64_t>(width) * 4u;
        const uint64_t bytes64 = stride64 * static_cast<uint64_t>(height);
        if (stride64 > std::numeric_limits<UINT>::max() ||
            bytes64 > std::numeric_limits<UINT>::max()) {
            hr = E_OUTOFMEMORY;
        } else {
            pixels.resize(static_cast<size_t>(bytes64));
            hr = converter->CopyPixels(
                nullptr, static_cast<UINT>(stride64),
                static_cast<UINT>(bytes64), pixels.data());
        }
    }

    ReleaseCom(converter);
    ReleaseCom(frame);
    ReleaseCom(decoder);
    ReleaseCom(stream);
    ReleaseCom(factory);
    if (uninitialize) CoUninitialize();

    if (FAILED(hr) || pixels.empty()) {
        error = "screenshot_png_decode_failed";
        pixels.clear();
        width = height = 0;
        return false;
    }
    return true;
}

} // namespace

ScreenshotWorkspace::~ScreenshotWorkspace() {
    ReleaseTexture();
}

void ScreenshotWorkspace::ReleaseTexture() {
    auto* view = static_cast<ID3D11ShaderResourceView*>(textureView_);
    if (view) view->Release();
    textureView_ = nullptr;
    textureWidth_ = textureHeight_ = 0;
}

bool ScreenshotWorkspace::UpdateTexture(UiContext& context, std::string& error) {
    error.clear();
    if (!context.screenshotModel || !context.nativeRenderDevice) {
        error = "screenshot_renderer_unavailable";
        return false;
    }
    if (loadedGeneration_ == context.screenshotModel->Generation())
        return textureView_ != nullptr;
    loadedGeneration_ = context.screenshotModel->Generation();

    ReleaseTexture();
    if (context.screenshotModel->ImageBase64().empty()) return true;

    std::vector<uint8_t> encoded;
    if (!DecodeBase64(context.screenshotModel->ImageBase64(), encoded, error))
        return false;

    std::vector<uint8_t> pixels;
    UINT width = 0, height = 0;
    if (!DecodePngRgba(encoded, pixels, width, height, error))
        return false;

    auto* device = static_cast<ID3D11Device*>(context.nativeRenderDevice);
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = pixels.data();
    initial.SysMemPitch = width * 4u;

    ID3D11Texture2D* texture = nullptr;
    HRESULT hr = device->CreateTexture2D(&desc, &initial, &texture);
    if (FAILED(hr) || !texture) {
        error = "screenshot_texture_create_failed";
        return false;
    }

    ID3D11ShaderResourceView* view = nullptr;
    hr = device->CreateShaderResourceView(texture, nullptr, &view);
    texture->Release();
    if (FAILED(hr) || !view) {
        error = "screenshot_texture_view_failed";
        return false;
    }

    textureView_ = view;
    textureWidth_ = width;
    textureHeight_ = height;
    return true;
}

void ScreenshotWorkspace::Draw(UiContext& context) {
    const auto session = context.sessions ? context.sessions->Active() : nullptr;
    if (!session || !context.screenshotModel) {
        ImGui::TextDisabled("Select a process to capture its visual state.");
        return;
    }

    if (targetId_ != session->Target().id) {
        targetId_ = session->Target().id;
        context.screenshotModel->Reset();
        loadedGeneration_ = 0;
        ReleaseTexture();
    }

    ImGui::TextUnformatted("Target screenshot");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    ImGui::Combo("##ScreenshotMode", &modeIndex_, kModes, IM_ARRAYSIZE(kModes));
    ImGui::SameLine();

    if (context.payload && !context.payload->Ready()) {
        if (ImGui::SmallButton("Connect existing")) {
            std::string error;
            if (!context.payload->TryConnectExisting(&error))
                context.status = "Runtime connect failed: " + error;
            else
                context.status = "Runtime connected";
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!context.mutationAllowed);
        if (ImGui::SmallButton("Enable runtime")) {
            std::string error;
            if (!context.payload->EnsureReady(&error))
                context.status = "Runtime enable failed: " + error;
            else
                context.status = "Runtime enabled";
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
    }

    ImGui::BeginDisabled(!context.payload || !context.payload->Ready());
    if (ImGui::Button("Capture")) {
        std::string error;
        if (!context.screenshotModel->Capture(kModes[modeIndex_], &error)) {
            context.status = "Screenshot failed: " + error;
        } else if (!UpdateTexture(context, error)) {
            context.status = "Screenshot decode failed: " + error;
        } else {
            context.status = context.screenshotModel->Meta();
        }
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::TextDisabled("%s", context.screenshotModel->Meta().empty()
                                 ? "No capture yet."
                                 : context.screenshotModel->Meta().c_str());

    ImGui::Separator();

    std::string textureError;
    if (loadedGeneration_ != context.screenshotModel->Generation())
        UpdateTexture(context, textureError);

    const ImVec2 available = ImGui::GetContentRegionAvail();
    if (!textureView_ || textureWidth_ == 0 || textureHeight_ == 0) {
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::max(20.0f, available.y * 0.35f));
        ImGui::TextDisabled(textureError.empty()
                                ? "Capture a live or cached target frame."
                                : textureError.c_str());
        return;
    }

    const float scale = std::min(
        available.x / static_cast<float>(textureWidth_),
        available.y / static_cast<float>(textureHeight_));
    const ImVec2 imageSize(
        std::max(1.0f, static_cast<float>(textureWidth_) * scale),
        std::max(1.0f, static_cast<float>(textureHeight_) * scale));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                         std::max(0.0f, (available.x - imageSize.x) * 0.5f));

    const ImTextureID textureId =
        static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(textureView_));
    ImGui::Image(ImTextureRef(textureId), imageSize);
}

} // namespace cortex::ui
