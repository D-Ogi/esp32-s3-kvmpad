// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once
#include <d3d11.h>
#include <winrt/base.h>
#include <vector>
#include "region.hpp"

struct TextureExtentMismatch : winrt::hresult_canceled {};

// Only the selected rectangle is copied to host-visible memory.
inline std::vector<uint32_t> crop_texture(ID3D11Device* gpu, ID3D11DeviceContext* context,
        ID3D11Texture2D* texture, int frame_w, int frame_h, const Region& region, int crop_w, int crop_h, int width, int height) {
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    // WGC coordinates refer to the upright, selected item's content. A stale frame-pool
    // texture can still contain the requested rectangle after a mode or orientation change.
    // Requiring the complete extent avoids indexing that texture in the new coordinates.
    if (frame_w<=0 || frame_h<=0 || desc.Width!=static_cast<UINT>(frame_w) ||
        desc.Height!=static_cast<UINT>(frame_h))
        throw TextureExtentMismatch{};
    if (desc.Format!=DXGI_FORMAT_B8G8R8A8_UNORM ||
        desc.SampleDesc.Count!=1 || desc.ArraySize!=1 || desc.MipLevels!=1 ||
        region.left<0 || region.top<0 || region.width<=0 || region.height<=0 ||
        int64_t(region.left)+region.width>frame_w || int64_t(region.top)+region.height>frame_h)
        throw winrt::hresult_canceled();
    UINT right=static_cast<UINT>(region.left+region.width);
    UINT bottom=static_cast<UINT>(region.top+region.height);
    D3D11_BOX box{static_cast<UINT>(region.left), static_cast<UINT>(region.top), 0, right, bottom, 1};
    desc.Width = region.width; desc.Height = region.height;
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; desc.MiscFlags = 0;
    winrt::com_ptr<ID3D11Texture2D> staging;
    winrt::check_hresult(gpu->CreateTexture2D(&desc, nullptr, staging.put()));
    context->CopySubresourceRegion(staging.get(), 0, 0, 0, 0, texture, 0, &box);
    std::vector<uint32_t> pixels(size_t(width) * height);
    D3D11_MAPPED_SUBRESOURCE map{};
    winrt::check_hresult(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &map));
    for (int y = 0; y < height; ++y) {
        int sy = y * crop_h / height - region.pad_y;
        if (sy < 0 || sy >= region.height) continue;
        auto row = reinterpret_cast<const uint32_t*>(static_cast<const char*>(map.pData) +
                                                     size_t(sy) * map.RowPitch);
        for (int x = 0; x < width; ++x) {
            int sx = x * crop_w / width - region.pad_x;
            if (sx >= 0 && sx < region.width) pixels[size_t(y) * width + x] = row[sx];
        }
    }
    context->Unmap(staging.get(), 0);
    return pixels;
}
