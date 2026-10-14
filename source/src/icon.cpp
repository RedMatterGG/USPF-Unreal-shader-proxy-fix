// USPF - Unreal shader proxy fix. Copyright (c) 2026 Meower. MIT License.
// Written by a FreeLLMAPI agent (gpt-5.6-luna), reviewed and integrated.
#include <windows.h>
#include <wincodec.h>
#include <shlobj.h>
#include <shellapi.h>

#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "icon.h"

namespace uspf {

std::vector<uint8_t> ExeIconPng(const std::wstring& exePath) {
    HICON hIcon = nullptr;
    HRESULT hr = SHDefExtractIconW(exePath.c_str(), 0, 0, &hIcon, nullptr, MAKELONG(256, 256));
    if (FAILED(hr) || !hIcon) {
        HICON largeIcon = nullptr;
        if (ExtractIconExW(exePath.c_str(), 0, &largeIcon, nullptr, 1) == 0 || !largeIcon) return {};
        hIcon = largeIcon;
    }

    IWICImagingFactory* factory = nullptr;
    IWICBitmap* bitmap = nullptr;
    IStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    std::vector<uint8_t> result;

    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) hr = factory->CreateBitmapFromHICON(hIcon, &bitmap);
    if (SUCCEEDED(hr)) hr = CreateStreamOnHGlobal(nullptr, TRUE, &stream);
    if (SUCCEEDED(hr)) hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (SUCCEEDED(hr)) hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, nullptr);
    if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
    if (SUCCEEDED(hr)) {
        UINT width = 0, height = 0;
        hr = bitmap->GetSize(&width, &height);
        if (SUCCEEDED(hr)) hr = frame->SetSize(width, height);
    }
    if (SUCCEEDED(hr)) {
        WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat32bppBGRA;
        hr = frame->SetPixelFormat(&pixelFormat);
    }
    if (SUCCEEDED(hr)) hr = frame->WriteSource(bitmap, nullptr);
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = encoder->Commit();
    if (SUCCEEDED(hr)) {
        STATSTG stat{};
        hr = stream->Stat(&stat, STATFLAG_NONAME);
        if (SUCCEEDED(hr) && stat.cbSize.HighPart == 0) {
            const size_t size = static_cast<size_t>(stat.cbSize.LowPart);
            HGLOBAL global = nullptr;
            hr = GetHGlobalFromStream(stream, &global);
            if (SUCCEEDED(hr) && global) {
                void* data = GlobalLock(global);
                if (data || size == 0) {
                    result.resize(size);
                    if (size != 0) std::memcpy(result.data(), data, size);
                    if (data) GlobalUnlock(global);
                } else {
                    hr = E_FAIL;
                }
            }
        } else if (SUCCEEDED(hr)) {
            hr = E_FAIL;
        }
    }

    if (frame) frame->Release();
    if (encoder) encoder->Release();
    if (stream) stream->Release();
    if (bitmap) bitmap->Release();
    if (factory) factory->Release();
    DestroyIcon(hIcon);
    return SUCCEEDED(hr) ? result : std::vector<uint8_t>{};
}

}  // namespace uspf
