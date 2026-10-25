#include "harness/screenshot.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

#include "core/log.h"
#include "game/rfg.h"
#include "hooks/hooks.h"

namespace screenshot {
namespace {

constexpr UINT kMaxWidth = 1280;
// keen::GraphicsSystem: the swap chain endFrame presents at +0x20348 (keen::RenderSwapChain*, null when nothing is
// presented); RenderSwapChain::pSwapChain (IDXGISwapChain*) at +0x13C.
constexpr uint32_t kGraphicsPresentSwapChain = 0x20348, kRenderSwapChainDxgi = 0x13C;

std::mutex g_mutex;
std::wstring g_pending;  // path of the requested screenshot, empty when none

bool(__cdecl* o_end_frame)(uint8_t*);

// --- PNG writer: RGB8, zlib stream of stored (uncompressed) deflate blocks.

uint32_t crc32(const uint8_t* p, size_t n, uint32_t c = 0xFFFFFFFFu) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t v = i;
            for (int k = 0; k < 8; ++k) v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            table[i] = v;
        }
        init = true;
    }
    for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c;
}

void put_be32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(uint8_t(v >> 24));
    out.push_back(uint8_t(v >> 16));
    out.push_back(uint8_t(v >> 8));
    out.push_back(uint8_t(v));
}

void chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
    put_be32(out, uint32_t(data.size()));
    const size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    put_be32(out, crc32(out.data() + start, out.size() - start) ^ 0xFFFFFFFFu);
}

bool write_png(const std::wstring& path, const std::vector<uint8_t>& rgb, uint32_t w, uint32_t h) {
    std::vector<uint8_t> raw;  // filter byte 0 + row
    raw.reserve(size_t(h) * (w * 3 + 1));
    for (uint32_t y = 0; y < h; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), rgb.begin() + ptrdiff_t(size_t(y) * w * 3), rgb.begin() + ptrdiff_t(size_t(y + 1) * w * 3));
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (uint8_t v : raw) {
        a = (a + v) % 65521;
        b = (b + a) % 65521;
    }
    for (size_t pos = 0; pos < raw.size() || raw.empty();) {
        const size_t len = std::min<size_t>(65535, raw.size() - pos);
        const bool last = pos + len >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(uint8_t(len));
        z.push_back(uint8_t(len >> 8));
        z.push_back(uint8_t(~len));
        z.push_back(uint8_t(~len >> 8));
        z.insert(z.end(), raw.begin() + ptrdiff_t(pos), raw.begin() + ptrdiff_t(pos + len));
        pos += len;
        if (last) break;
    }
    put_be32(z, (b << 16) | a);

    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<uint8_t> ihdr;
    put_be32(ihdr, w);
    put_be32(ihdr, h);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(f, png.data(), DWORD(png.size()), &written, nullptr) && written == png.size();
    CloseHandle(f);
    return ok;
}

float half_to_float(uint16_t h) {
    const uint32_t e = (h >> 10) & 0x1F, m = h & 0x3FF;
    float v = e == 0 ? std::ldexp(float(m), -24) : e == 31 ? 65504.f : std::ldexp(float(m | 0x400), int(e) - 25);
    return (h & 0x8000) ? -v : v;
}

uint8_t to8(float v) { return uint8_t(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f); }

// Reads pixel (x, y) of a mapped texture as 8-bit RGB; false for unsupported formats.
bool pixel(DXGI_FORMAT fmt, const uint8_t* row, uint32_t x, uint8_t* out) {
    switch (fmt) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            memcpy(out, row + x * 4, 3);
            return true;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
            out[0] = row[x * 4 + 2];
            out[1] = row[x * 4 + 1];
            out[2] = row[x * 4];
            return true;
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: {
            uint32_t v;
            memcpy(&v, row + x * 4, 4);
            out[0] = uint8_t((v & 0x3FF) >> 2);
            out[1] = uint8_t(((v >> 10) & 0x3FF) >> 2);
            out[2] = uint8_t(((v >> 20) & 0x3FF) >> 2);
            return true;
        }
        case DXGI_FORMAT_R16G16B16A16_FLOAT: {
            uint16_t c[3];
            memcpy(c, row + x * 8, 6);
            for (int i = 0; i < 3; ++i) out[i] = to8(std::pow(std::max(half_to_float(c[i]), 0.f), 1.f / 2.2f));
            return true;
        }
        default:
            return false;
    }
}

void capture(IDXGISwapChain* sc, const std::wstring& path) {
    ID3D11Texture2D* back = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) || !back) {
        LOG("screenshot: no D3D11 back buffer");
        return;
    }
    D3D11_TEXTURE2D_DESC desc;
    back->GetDesc(&desc);
    ID3D11Device* dev = nullptr;
    back->GetDevice(&dev);
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);

    ID3D11Texture2D* source = back;
    ID3D11Texture2D* resolved = nullptr;
    if (desc.SampleDesc.Count > 1) {
        D3D11_TEXTURE2D_DESC rd = desc;
        rd.SampleDesc = {1, 0};
        rd.BindFlags = 0;
        rd.MiscFlags = 0;
        if (SUCCEEDED(dev->CreateTexture2D(&rd, nullptr, &resolved))) {
            ctx->ResolveSubresource(resolved, 0, back, 0, desc.Format);
            source = resolved;
        }
    }
    D3D11_TEXTURE2D_DESC sd = desc;
    sd.SampleDesc = {1, 0};
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    ID3D11Texture2D* staging = nullptr;
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, &staging))) {
        ctx->CopyResource(staging, source);
        if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
            const uint32_t step = (desc.Width + kMaxWidth - 1) / kMaxWidth;
            const uint32_t w = desc.Width / step, h = desc.Height / step;
            std::vector<uint8_t> rgb(size_t(w) * h * 3);
            bool ok = true;
            for (uint32_t y = 0; y < h && ok; ++y) {
                const auto* row = static_cast<const uint8_t*>(m.pData) + size_t(y * step) * m.RowPitch;
                for (uint32_t x = 0; x < w && ok; ++x) ok = pixel(desc.Format, row, x * step, &rgb[(size_t(y) * w + x) * 3]);
            }
            ctx->Unmap(staging, 0);
            if (!ok)
                LOG("screenshot: back buffer format %d not supported", int(desc.Format));
            else if (write_png(path, rgb, w, h))
                LOG("screenshot: %ls (%ux%u from %ux%u)", path.c_str(), w, h, desc.Width, desc.Height);
        }
        staging->Release();
    }
    if (resolved) resolved->Release();
    ctx->Release();
    dev->Release();
    back->Release();
}

// keen::graphics::endFrame(GraphicsSystem*) presents the finished frame (IDXGISwapChain::Present); the back buffer
// is copied just before that, on the render thread that owns the immediate context.
bool __cdecl d_end_frame(uint8_t* graphics) {
    bool wanted;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        wanted = !g_pending.empty();
    }
    if (wanted && graphics) {
        auto* rsc = *reinterpret_cast<uint8_t**>(graphics + kGraphicsPresentSwapChain);
        auto* sc = rsc ? *reinterpret_cast<IDXGISwapChain**>(rsc + kRenderSwapChainDxgi) : nullptr;
        if (sc) {
            std::wstring path;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                path.swap(g_pending);
            }
            if (!path.empty()) capture(sc, path);
        }
    }
    return o_end_frame(graphics);
}

}  // namespace

bool install() {
    static bool installed = false;  // the harness and the tests that take screenshots may each ask
    if (!installed)
        installed = hooks::create(rfg::rva::keen__graphics__endFrame, &d_end_frame, &o_end_frame,
                                  "keen::graphics::endFrame");
    return installed;
}

void request(const std::wstring& png_path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_pending = png_path;
}

}  // namespace screenshot
