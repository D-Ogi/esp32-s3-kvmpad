// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once
#include <wincodec.h>
#include <vector>
#include <winrt/base.h>

namespace jpeg {
inline std::vector<uint8_t> encode(uint32_t* pixels, unsigned width, unsigned height) {
    using namespace winrt;
    auto factory = create_instance<IWICImagingFactory>(CLSID_WICImagingFactory);
    com_ptr<IWICBitmap> source;
    check_hresult(factory->CreateBitmapFromMemory(width, height, GUID_WICPixelFormat32bppBGR,
        width * 4, width * height * 4, reinterpret_cast<BYTE*>(pixels), source.put()));
    com_ptr<IWICFormatConverter> rgb;
    check_hresult(factory->CreateFormatConverter(rgb.put()));
    check_hresult(rgb->Initialize(source.get(), GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone,
                                 nullptr, 0, WICBitmapPaletteTypeCustom));
    // Refuse the whole picture when none of these encoder qualities meets the wire budget.
    for (float quality : {.82f, .65f, .45f, .25f}) {
        com_ptr<IStream> stream;
        check_hresult(CreateStreamOnHGlobal(nullptr, TRUE, stream.put()));
        com_ptr<IWICBitmapEncoder> encoder;
        check_hresult(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, encoder.put()));
        check_hresult(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache));
        com_ptr<IWICBitmapFrameEncode> frame;
        com_ptr<IPropertyBag2> options;
        check_hresult(encoder->CreateNewFrame(frame.put(), options.put()));
        PROPBAG2 field{};
        field.pstrName = const_cast<wchar_t*>(L"ImageQuality");
        VARIANT value{};
        value.vt = VT_R4; value.fltVal = quality;
        check_hresult(options->Write(1, &field, &value));
        check_hresult(frame->Initialize(options.get()));
        check_hresult(frame->SetSize(width, height));
        auto format = GUID_WICPixelFormat24bppBGR;
        check_hresult(frame->SetPixelFormat(&format));
        if (format != GUID_WICPixelFormat24bppBGR) throw hresult_error(E_UNEXPECTED);
        check_hresult(frame->WriteSource(rgb.get(), nullptr));
        check_hresult(frame->Commit());
        check_hresult(encoder->Commit());
        STATSTG stat{};
        check_hresult(stream->Stat(&stat, STATFLAG_NONAME));
        if (stat.cbSize.QuadPart > 98304 || stat.cbSize.QuadPart == 0) continue;
        std::vector<uint8_t> bytes(static_cast<size_t>(stat.cbSize.QuadPart));
        check_hresult(stream->Seek({}, STREAM_SEEK_SET, nullptr));
        ULONG read = 0;
        check_hresult(stream->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &read));
        if (read != bytes.size()) throw hresult_error(E_UNEXPECTED);
        return bytes;
    }
    return {};
}
}
