// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
// Test executable only. All textures originate in this process; no desktop or picker API.
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <dxgi1_2.h>
#include <array>
#include <utility>
#include <stdexcept>
#include <fcntl.h>
#include <io.h>
#include <string>
#include "crop_texture.hpp"
#include "capture_output.hpp"

namespace {
struct Monitor { int left, top, width, height, dpi; };

uint32_t pattern(int x, int y) {
    unsigned gray = 24 + 32 * ((x / 64 + 3 * (y / 64)) % 7);
    return 0xff000000u | (gray * 0x010101u);
}

void write_bytes(const void* data, size_t size) {
    if (fwrite(data, 1, size, stdout) != size || fflush(stdout)) throw winrt::hresult_canceled();
}

// The producer writes a native landscape raster through a forward rotation into an upright
// synthetic WGC surface. The consumer oracle below independently rotates coordinates back.
uint32_t address_color(unsigned x, unsigned y, unsigned source) {
    uint32_t address=(y<<12)|x;
    return 0xff000000u | ((address*0x75b9d5u + source*0x19660du) & 0xffffffu);
}

winrt::com_ptr<ID3D11Texture2D> oriented_surface(ID3D11Device* gpu, int w, int h, unsigned turn, unsigned source) {
    int sw=turn%2 ? h:w, sh=turn%2 ? w:h;
    std::vector<uint32_t> raster(size_t(sw)*sh);
    for (int v=0;v<h;v++) for (int u=0;u<w;u++) {
        int x=u,y=v;
        switch (turn) {
        case 1: x=h-1-v; y=u; break;
        case 2: x=w-1-u; y=h-1-v; break;
        case 3: x=v; y=w-1-u; break;
        }
        raster[size_t(y)*sw+x]=address_color(u,v,source);
    }
    D3D11_TEXTURE2D_DESC d{};
    d.Width=sw; d.Height=sh; d.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
    d.ArraySize=d.MipLevels=d.SampleDesc.Count=1; d.Usage=D3D11_USAGE_DEFAULT;
    D3D11_SUBRESOURCE_DATA initial{raster.data(),UINT(sw*sizeof(uint32_t)),0};
    winrt::com_ptr<ID3D11Texture2D> texture;
    winrt::check_hresult(gpu->CreateTexture2D(&d,&initial,texture.put()));
    return texture;
}

void coordinate_proof(bool hardware) {
    winrt::com_ptr<ID3D11Device> gpu;
    winrt::com_ptr<ID3D11DeviceContext> context;
    winrt::check_hresult(D3D11CreateDevice(nullptr,hardware?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP,
        nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,gpu.put(),nullptr,context.put()));
    winrt::com_ptr<IDXGIAdapter> adapter;
    winrt::check_hresult(gpu.as<IDXGIDevice>()->GetAdapter(adapter.put()));
    DXGI_ADAPTER_DESC1 info{};
    winrt::check_hresult(adapter.as<IDXGIAdapter1>()->GetDesc1(&info));
    if (hardware && (info.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) throw std::runtime_error("software adapter");
    printf("adapter=%s vendor=%04x device=%04x software=%u\n",hardware?"hardware":"warp",
           info.VendorId,info.DeviceId,!!(info.Flags & DXGI_ADAPTER_FLAG_SOFTWARE));
    unsigned frames=0, clamped=0, padded=0, rejected=0;
    uint64_t pixels=0;
    unsigned mode_frames[2][3]{};
    for (auto [nw,nh]: {std::pair{2003,1111},std::pair{399,301}}) {
        for (unsigned turn=0;turn<4;turn++) {
            int sw=turn%2?nh:nw, sh=turn%2?nw:nh;
            const int left=turn<2 ? -sw-29 : 1501, top=turn%2 ? -801 : 127;
            auto selected=oriented_surface(gpu.get(),nw,nh,turn,turn+1);
            for (auto [ow,oh]: {std::pair{512,512},std::pair{960,540}}) {
                for (int factor: {1,2,4}) {
                    int cw=ow*factor/2,ch=oh*factor/2;
                    // Include positions one pixel before, on and beyond the clamp threshold.
                    int middle_x=sw/2+3,middle_y=sh/2-1;
                    std::array<std::pair<int,int>,13> cursors{{{0,0},{sw-1,0},{0,sh-1},{sw-1,sh-1},
                        {middle_x,middle_y},{0,middle_y},{sw-1,middle_y},{middle_x,0},{middle_x,sh-1},
                        {cw/2-1,middle_y},{cw/2,middle_y},{middle_x,ch/2-1},{middle_x,ch/2}}};
                    for (auto [x,y]:cursors) {
                        if (x>=sw || y>=sh) continue;
                        Region r{};
                        if (!cursor_region(left,top,left+sw,top+sh,left+x,top+y,sw,sh,cw,ch,r))
                            throw std::runtime_error("in-monitor cursor rejected");
                        int rw=std::min(sw,cw), rh=std::min(sh,ch);
                        int wanted_x=x-cw/2,wanted_y=y-ch/2;
                        int ox=std::max(0,std::min(sw-rw,wanted_x)),oy=std::max(0,std::min(sh-rh,wanted_y));
                        bool edge=cw>sw || ch>sh || ox!=wanted_x || oy!=wanted_y;
                        if (r.left!=ox || r.top!=oy || r.width!=rw || r.height!=rh || r.clamped!=edge ||
                            r.pad_x!=(cw-rw)/2 || r.pad_y!=(ch-rh)/2) throw std::runtime_error("incorrect crop metadata");
                        auto output=crop_texture(gpu.get(),context.get(),selected.get(),sw,sh,r,cw,ch,ow,oh);
                        for (int j=0;j<oh;j++) for (int i=0;i<ow;i++) {
                            int sx=ox+i*factor/2-(cw-rw)/2, sy=oy+j*factor/2-(ch-rh)/2;
                            uint32_t expected=0;
                            if (sx>=0 && sx<sw && sy>=0 && sy<sh) {
                                int u=sx,v=sy;
                                switch (turn) {
                                case 1: u=sy; v=nh-1-sx; break;
                                case 2: u=nw-1-sx; v=nh-1-sy; break;
                                case 3: u=nw-1-sy; v=sx; break;
                                }
                                expected=address_color(u,v,turn+1);
                            }
                            if (output[size_t(j)*ow+i]!=expected) throw std::runtime_error("coordinate pixel mismatch");
                        }
                        mode_frames[ow==960][factor==4?2:factor-1]++;
                        frames++; pixels+=size_t(ow)*oh; clamped+=edge; padded+=rw<cw || rh<ch;
                    }
                }
            }
            Region r{0,0,16,16,0,0,false};
            // The crop fits each claimed size, but none matches the full texture.
            for (auto [w,h]: {std::pair{sh,sw},std::pair{sw-1,sh},std::pair{sw,sh+1}}) {
                try { crop_texture(gpu.get(),context.get(),selected.get(),w,h,r,16,16,16,16); }
                catch (const TextureExtentMismatch&) { rejected++; continue; }
                throw std::runtime_error("mismatched texture was admitted");
            }
            for (auto [x,y]: {std::pair{-1,0},std::pair{sw,0},std::pair{0,-1},std::pair{0,sh}}) {
                if (cursor_region(left,top,left+sw,top+sh,left+x,top+y,sw,sh,16,16,r))
                    throw std::runtime_error("unselected coordinate admitted");
                rejected++;
            }
        }
    }
    for (unsigned format=0;format<2;format++) {
        unsigned w=format?960:512,h=format?540:512;
        for (unsigned mode=0;mode<3;mode++) {
            unsigned factor=1u<<mode,n=mode_frames[format][mode];
            printf("source=%ux%u output=%ux%u frames=%u exact_bgra=%llu\n",w*factor/2,h*factor/2,w,h,n,
                static_cast<unsigned long long>(n)*w*h);
        }
    }
    printf("frames=%u exact_bgra=%llu clamped=%u padded=%u refused=%u orientations=0/90/180/270\n",
        frames,static_cast<unsigned long long>(pixels),clamped,padded,rejected);
}

int run(int argc, char** argv) {
    if (argc != 5 || std::string(argv[3]) != "--parent") return 2;
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    char layout[32]{};
    if (!GetEnvironmentVariableA("KVMPAD_TEST_LAYOUT", layout, sizeof(layout))) return 2;
    const std::string scenario(layout);
    if (scenario == "denied") return 3;
    Monitor monitor{-1440, -180, 1440, 1080, 144};
    if (scenario == "portrait") monitor = {1920, -250, 900, 1500, 120};
    if (scenario == "small") monitor = {-399, 350, 399, 301, 96};
    const bool ruler = scenario == "horizontal-ruler";
    if (ruler) monitor = {-2048, -180, 2048, 1280, 144};
    auto pixel = [ruler](int x, int y) -> uint32_t {
        if (!ruler) return pattern(x, y);
        bool line = (y >= 574 && y < 586) || (y >= 640 && y < 656) || (y >= 728 && y < 750);
        return line ? 0xffe8e8e8u : 0xff181818u;
    };
    const std::string codec(argv[1]);
    int width = codec == "cinepak-intra" ? 960 : 512;
    int height = width == 960 ? 540 : 512;
    int factor = atoi(argv[2]);
    if (factor != 1 && factor != 2 && factor != 4) return 2;
    int crop_w = width * factor / 2, crop_h = height * factor / 2;

    winrt::init_apartment();
    winrt::com_ptr<ID3D11Device> gpu;
    winrt::com_ptr<ID3D11DeviceContext> context;
    char driver[16]{};
    GetEnvironmentVariableA("KVMPAD_TEST_GPU", driver, sizeof(driver));
    bool hardware=std::string(driver)=="hardware";
    if (*driver && !hardware && std::string(driver)!="warp") return 2;
    winrt::check_hresult(D3D11CreateDevice(nullptr, hardware ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, gpu.put(), nullptr, context.put()));
    std::vector<uint32_t> source(size_t(monitor.width) * monitor.height);
    for (int y = 0; y < monitor.height; ++y)
        for (int x = 0; x < monitor.width; ++x)
            source[size_t(y) * monitor.width + x] = pixel(x, y);
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = monitor.width; desc.Height = monitor.height; desc.MipLevels = desc.ArraySize = 1;
    desc.SampleDesc.Count = 1; desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.Usage = D3D11_USAGE_DEFAULT;
    D3D11_SUBRESOURCE_DATA initial{source.data(), UINT(monitor.width * sizeof(uint32_t)), 0};
    winrt::com_ptr<ID3D11Texture2D> texture;
    winrt::check_hresult(gpu->CreateTexture2D(&desc, &initial, texture.put()));
    constexpr char name[] = R"(\\.\DISPLAY42)" "\n";
    if (scenario == "unidentified") write_bytes("Unknown\n", 8);
    else write_bytes(name, sizeof(name) - 1);
    for (int index = 0;;) {
        int request = getchar();
        if (request == EOF) return 0;
        if (request == 'P') continue;
        if (request != 'F' && request != 'M') return 2;
        // The first cursor sample is in device-independent pixels, converted once.
        int x = MulDiv(monitor.width * 96 / monitor.dpi / 2 + 16, monitor.dpi, 96);
        int y = MulDiv(monitor.height * 96 / monitor.dpi / 2 - 8, monitor.dpi, 96);
        if (index == 1) { x = monitor.width - 1; y = monitor.height - 1; }
        if (index == 2) { x = 0; y = 0; }
        if (index == 3) x = monitor.width; // now over the adjacent, unselected display
        if (scenario=="paced") {
            // Encode a synthetic source clock in the cursor. Both test processes can
            // independently read GetTickCount64; no cursor or desktop APIs are used.
            auto tick=(GetTickCount64()/10)%(monitor.width*monitor.height);
            x=int(tick%monitor.width); y=int(tick/monitor.width);
        }
        if (ruler) {
            constexpr int moves[]{1, 2, 4, 8, 16, 32, 16, 8, 4, 2, 1, 0, -4, -16, 0};
            x = 1024;
            y = 640 + (index >= 150 && index < 165 ? moves[index - 150] : 0);
        }
        Region region{};
        int frame_w = monitor.width - (scenario == "resized" ? 1 : 0);
        if (!cursor_region(monitor.left, monitor.top, monitor.left + monitor.width, monitor.top + monitor.height,
                monitor.left + x, monitor.top + y, frame_w, monitor.height, crop_w, crop_h, region))
            return 3;
        auto pixels = crop_texture(gpu.get(), context.get(), texture.get(), monitor.width, monitor.height, region, crop_w, crop_h, width, height);
        // Independent source-coordinate oracle, including padding when a display is smaller.
        auto origin = [](int cursor, int extent, int crop) {
            return extent < crop ? -((crop - extent) / 2) : std::clamp(cursor - crop / 2, 0, extent - crop);
        };
        int left = origin(x, monitor.width, crop_w), top = origin(y, monitor.height, crop_h);
        for (int oy = 0; oy < height; ++oy)
            for (int ox = 0; ox < width; ++ox) {
                int sx = left + ox * factor / 2, sy = top + oy * factor / 2;
                bool inside = sx >= 0 && sx < monitor.width && sy >= 0 && sy < monitor.height;
                if (pixels[size_t(oy) * width + ox] != (inside ? pixel(sx, sy) : 0u)) return 4;
            }
        publish_frame(pixels, width, height, codec == "jpeg", [&] {
            if (scenario == "revoked") throw winrt::hresult_canceled();
            if (request=='M') {
                auto prefix=crop_prefix(monitor.left,monitor.top,monitor.left+x,monitor.top+y,region);
                write_bytes(&prefix,sizeof(prefix));
            }
        }, write_bytes);
        ++index;
    }
}
}

int main(int argc, char** argv) {
    try {
        if (argc == 2) {
            const std::string command = argv[1];
            const bool jpeg = command == "--encode-jpeg";
            if (!jpeg && command != "--encode-cinepak") return 2;
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            _setmode(_fileno(stdin), _O_BINARY);
            _setmode(_fileno(stdout), _O_BINARY);
            const int width = jpeg ? 512 : cinepak::width, height = jpeg ? 512 : cinepak::height;
            std::vector<uint32_t> pixels(width * height);
            size_t count;
            while ((count = fread(pixels.data(), 1, pixels.size() * sizeof(uint32_t), stdin)) != 0) {
                if (count != pixels.size() * sizeof(uint32_t)) return 2;
                publish_frame(pixels, width, height, jpeg, [] {}, write_bytes);
            }
            return ferror(stdin) ? 2 : 0;
        }
        if (argc==3 && std::string(argv[1])=="--coordinate-proof") {
            std::string driver=argv[2];
            if (driver!="hardware" && driver!="warp") return 2;
            coordinate_proof(driver=="hardware");
            return 0;
        }
        return run(argc, argv);
    } catch (const winrt::hresult_error& e) {
        fprintf(stderr,"HRESULT 0x%08x\n",static_cast<unsigned>(e.code().value));
        return 3;
    } catch (const std::exception& e) { fprintf(stderr,"%s\n",e.what()); return 3; }
      catch (...) { return 3; }
}
