#include "test/memory_census.h"

#include <windows.h>
#include <d3d11.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "core/session.h"
#include "hooks/hooks.h"

namespace memory_census {
namespace {

constexpr double kMb = 1048576.0;

// ---- D3D11 resources alive ----------------------------------------------------------------------------------------

// Every buffer and 2D texture the game creates carries a private-data object of ours (SetPrivateDataInterface), which
// the runtime lets go of when the resource is destroyed: its bytes count while it lives. Textures by their texels and
// mips (block-compressed formats at their rate).
enum Kind {
    kGeometryImmutable,
    kGeometryDefault,
    kGeometryDynamic,
    kConstants,
    kOtherBuffers,
    kTextures,
    kStaging,
    kKinds
};
const char* const kKindNames[kKinds] = {"vertex/index immutable", "vertex/index default", "vertex/index dynamic",
                                        "constant buffers",       "other buffers",        "2D textures",
                                        "staging"};
std::atomic<int64_t> g_live_bytes[kKinds];
std::atomic<int32_t> g_live[kKinds];
std::atomic<uint32_t> g_made[kKinds];
int64_t g_live_bytes_last[kKinds];
int32_t g_live_last[kKinds];
uint32_t g_made_last[kKinds];
bool g_device_hooked = false;

constexpr GUID kTrackerGuid = {0x6b0d1f3a, 0x52c4, 0x4e1b, {0x9a, 0x77, 0x31, 0x2e, 0x8c, 0x51, 0x0f, 0xd2}};

class Tracker final : public IUnknown {
public:
    Tracker(Kind kind, int64_t bytes) : kind_(kind), bytes_(bytes) {
        g_live_bytes[kind] += bytes;
        ++g_live[kind];
        ++g_made[kind];
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (riid != __uuidof(IUnknown)) return E_NOINTERFACE;
        *out = static_cast<IUnknown*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --refs_;
        if (!left) {
            g_live_bytes[kind_] -= bytes_;
            --g_live[kind_];
            delete this;
        }
        return left;
    }

private:
    std::atomic<ULONG> refs_{1};
    Kind kind_;
    int64_t bytes_;
};

void track(ID3D11DeviceChild* resource, Kind kind, int64_t bytes) {
    auto* t = new Tracker(kind, bytes);
    resource->SetPrivateDataInterface(kTrackerGuid, t);  // takes a reference of its own
    t->Release();
}

using CreateBufferFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_BUFFER_DESC*,
                                                   const D3D11_SUBRESOURCE_DATA*, ID3D11Buffer**);
using CreateTexture2DFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*,
                                                      const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
CreateBufferFn o_create_buffer;
CreateTexture2DFn o_create_texture_2d;

HRESULT STDMETHODCALLTYPE d_create_buffer(ID3D11Device* device, const D3D11_BUFFER_DESC* desc,
                                          const D3D11_SUBRESOURCE_DATA* data, ID3D11Buffer** out) {
    const HRESULT hr = o_create_buffer(device, desc, data, out);
    if (SUCCEEDED(hr) && desc && out && *out) {
        Kind kind = kOtherBuffers;
        if (desc->Usage == D3D11_USAGE_STAGING)
            kind = kStaging;
        else if (desc->BindFlags & D3D11_BIND_CONSTANT_BUFFER)
            kind = kConstants;
        else if (desc->BindFlags & (D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER))
            kind = desc->Usage == D3D11_USAGE_IMMUTABLE ? kGeometryImmutable
                   : desc->Usage == D3D11_USAGE_DYNAMIC ? kGeometryDynamic
                                                        : kGeometryDefault;
        track(*out, kind, desc->ByteWidth);
    }
    return hr;
}

// Bits per texel; block-compressed formats per texel of their 4x4 blocks; 32 for the formats not listed.
uint32_t texel_bits(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_BC1_TYPELESS: case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC4_TYPELESS: case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC4_SNORM:
        return 4;
    case DXGI_FORMAT_BC2_TYPELESS: case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_TYPELESS: case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC5_TYPELESS: case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC5_SNORM:
    case DXGI_FORMAT_BC6H_TYPELESS: case DXGI_FORMAT_BC6H_UF16: case DXGI_FORMAT_BC6H_SF16:
    case DXGI_FORMAT_BC7_TYPELESS: case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB:
    case DXGI_FORMAT_R8_TYPELESS: case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_UINT: case DXGI_FORMAT_R8_SNORM:
    case DXGI_FORMAT_R8_SINT: case DXGI_FORMAT_A8_UNORM:
        return 8;
    case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_D16_UNORM: case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_UINT: case DXGI_FORMAT_R16_SNORM: case DXGI_FORMAT_R16_SINT: case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R8G8_UINT: case DXGI_FORMAT_R8G8_SNORM: case DXGI_FORMAT_R8G8_SINT:
    case DXGI_FORMAT_B5G6R5_UNORM: case DXGI_FORMAT_B5G5R5A1_UNORM:
        return 16;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UINT: case DXGI_FORMAT_R16G16B16A16_SNORM: case DXGI_FORMAT_R16G16B16A16_SINT:
    case DXGI_FORMAT_R32G32_TYPELESS: case DXGI_FORMAT_R32G32_FLOAT: case DXGI_FORMAT_R32G32_UINT:
    case DXGI_FORMAT_R32G32_SINT:
        return 64;
    case DXGI_FORMAT_R32G32B32_TYPELESS: case DXGI_FORMAT_R32G32B32_FLOAT: case DXGI_FORMAT_R32G32B32_UINT:
    case DXGI_FORMAT_R32G32B32_SINT:
        return 96;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R32G32B32A32_UINT:
    case DXGI_FORMAT_R32G32B32A32_SINT:
        return 128;
    default:
        return 32;
    }
}

HRESULT STDMETHODCALLTYPE d_create_texture_2d(ID3D11Device* device, const D3D11_TEXTURE2D_DESC* desc,
                                              const D3D11_SUBRESOURCE_DATA* data, ID3D11Texture2D** out) {
    const HRESULT hr = o_create_texture_2d(device, desc, data, out);
    if (SUCCEEDED(hr) && desc && out && *out) {
        const uint32_t bits = texel_bits(desc->Format);
        uint64_t bytes = 0;
        uint32_t w = std::max(1u, desc->Width), h = std::max(1u, desc->Height);
        for (uint32_t m = 0;; ++m) {  // MipLevels 0: the whole chain
            bytes += uint64_t(w) * h * bits / 8;
            if ((desc->MipLevels && m + 1 >= desc->MipLevels) || (w == 1 && h == 1)) break;
            w = std::max(1u, w / 2);
            h = std::max(1u, h / 2);
        }
        bytes *= uint64_t(std::max(1u, desc->ArraySize)) * std::max(1u, desc->SampleDesc.Count);
        track(*out, desc->Usage == D3D11_USAGE_STAGING ? kStaging : kTextures, int64_t(bytes));
    }
    return hr;
}

using CreateDeviceFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT,
                                        UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
CreateDeviceFn o_create_device;

HRESULT WINAPI d_create_device(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, HMODULE sw, UINT flags,
                               const D3D_FEATURE_LEVEL* levels, UINT n_levels, UINT sdk, ID3D11Device** device,
                               D3D_FEATURE_LEVEL* level, ID3D11DeviceContext** context) {
    const HRESULT hr = o_create_device(adapter, type, sw, flags, levels, n_levels, sdk, device, level, context);
    if (SUCCEEDED(hr) && device && *device && !g_device_hooked) {
        g_device_hooked = true;
        void** vt = *reinterpret_cast<void***>(*device);
        // ID3D11Device vtable: 3 CreateBuffer, 5 CreateTexture2D.
        const bool ok = hooks::create_at(vt[3], reinterpret_cast<void*>(&d_create_buffer),
                                         reinterpret_cast<void**>(&o_create_buffer), "CreateBuffer") &&
                        hooks::enable_at(vt[3]) &&
                        hooks::create_at(vt[5], reinterpret_cast<void*>(&d_create_texture_2d),
                                         reinterpret_cast<void**>(&o_create_texture_2d), "CreateTexture2D") &&
                        hooks::enable_at(vt[5]);
        LOG("memory census: D3D11 device created; its buffers and 2D textures are %s",
            ok ? "counted while they live" : "NOT counted (hooks failed)");
    }
    return hr;
}

// ---- private memory by allocation ---------------------------------------------------------------------------------

struct Alloc {
    uint64_t committed = 0, size = 0;  // size: the whole reservation
};
std::unordered_map<uintptr_t, Alloc> g_allocs_last;
bool g_have_last = false;

std::unordered_map<uintptr_t, Alloc> private_allocations() {
    std::unordered_map<uintptr_t, Alloc> out;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t p = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
    const uintptr_t end = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
    MEMORY_BASIC_INFORMATION m;
    while (p < end && VirtualQuery(reinterpret_cast<void*>(p), &m, sizeof(m))) {
        if (m.State != MEM_FREE && m.Type == MEM_PRIVATE) {
            Alloc& a = out[reinterpret_cast<uintptr_t>(m.AllocationBase)];
            a.size += m.RegionSize;
            if (m.State == MEM_COMMIT) a.committed += m.RegionSize;
        }
        const uintptr_t next = reinterpret_cast<uintptr_t>(m.BaseAddress) + m.RegionSize;
        if (next <= p) break;
        p = next;
    }
    return out;
}

// ---- heaps --------------------------------------------------------------------------------------------------------

using HeapSummaryFn = BOOL(WINAPI*)(HANDLE, DWORD, HEAP_SUMMARY*);

// Busy blocks by size: below 64 KB in 8-byte steps, then in 64 KB steps (the last step holds the rest).
constexpr uint32_t kSmallSteps = 8192, kLargeSteps = 1024;
struct Histogram {
    uint32_t fine[kSmallSteps];   // "small" is a macro in the Windows headers
    uint32_t coarse[kLargeSteps];
    uint64_t blocks, bytes;
};
Histogram g_walk;  // filled with the heap locked: nothing is allocated meanwhile
constexpr size_t kMaxRegions = 2048;
uintptr_t g_regions[kMaxRegions];  // allocation bases of the walked heap's segments and of its blocks of their own
size_t g_region_count = 0;

struct HeapLast {
    SIZE_T allocated = 0, committed = 0;
    std::unique_ptr<Histogram> hist;
};
std::unordered_map<HANDLE, HeapLast> g_heaps_last;
std::unordered_map<uintptr_t, int> g_heap_of;  // allocation base -> the heap's place in GetProcessHeaps' list

void note_region(const void* p) {
    MEMORY_BASIC_INFORMATION m;
    if (g_region_count < kMaxRegions && VirtualQuery(p, &m, sizeof(m)))
        g_regions[g_region_count++] = reinterpret_cast<uintptr_t>(m.AllocationBase);
}

// A 32-bit NT heap's header: its first segment's signature (+0x08) and the heap's flags (+0x40). A heap made with
// HEAP_NO_SERIALIZE has no lock to walk it under, and another kind of heap is not walked either.
constexpr uint32_t kHeapSegmentSignature = 0x08, kHeapFlags = 0x40, kNtHeapSignature = 0xFFEEFFEEu;
bool walkable(HANDLE heap) {
    const auto* h = static_cast<const uint8_t*>(heap);
    return *reinterpret_cast<const uint32_t*>(h + kHeapSegmentSignature) == kNtHeapSignature &&
           !(*reinterpret_cast<const uint32_t*>(h + kHeapFlags) & HEAP_NO_SERIALIZE);
}

// With the heap locked: other threads wait on it meanwhile (a test only), and nothing here allocates.
bool walk(HANDLE heap, uint64_t& entries) {
    memset(&g_walk, 0, sizeof(g_walk));
    g_region_count = 0;
    entries = 0;
    if (!walkable(heap) || !HeapLock(heap)) return false;
    constexpr uint64_t kMaxEntries = 40000000;
    PROCESS_HEAP_ENTRY e{};
    while (entries < kMaxEntries && HeapWalk(heap, &e)) {
        ++entries;
        if (e.wFlags & PROCESS_HEAP_REGION) {
            note_region(e.lpData);
        } else if (e.wFlags & PROCESS_HEAP_ENTRY_BUSY) {
            const uint32_t size = e.cbData;
            ++g_walk.blocks;
            g_walk.bytes += size;
            if (size < 65536)
                ++g_walk.fine[size / 8];
            else
                ++g_walk.coarse[std::min<uint32_t>(size / 65536, kLargeSteps - 1)];
            if (size >= 512 * 1024) note_region(e.lpData);  // a block of its own (VirtualAlloc)
        }
    }
    HeapUnlock(heap);
    return true;
}

std::string size_text(uint64_t bytes) {
    char t[32];
    if (bytes < 65536)
        snprintf(t, sizeof(t), "%llu B", static_cast<unsigned long long>(bytes));
    else
        snprintf(t, sizeof(t), "%llu KB", static_cast<unsigned long long>(bytes / 1024));
    return t;
}

// The sizes whose blocks grew most since `before` (or, without it, hold the most bytes).
std::string size_changes(const Histogram& now, const Histogram* before) {
    struct Change {
        int64_t bytes, blocks;
        uint64_t size;
    };
    std::vector<Change> changes;
    auto add = [&](uint32_t n, uint32_t b, uint64_t size) {
        const int64_t d = before ? int64_t(n) - int64_t(b) : int64_t(n);
        if (d > 0) changes.push_back({d * int64_t(size), d, size});
    };
    for (uint32_t k = 0; k < kSmallSteps; ++k) add(now.fine[k], before ? before->fine[k] : 0, uint64_t(k) * 8);
    for (uint32_t k = 1; k < kLargeSteps; ++k) add(now.coarse[k], before ? before->coarse[k] : 0, uint64_t(k) * 65536);
    std::sort(changes.begin(), changes.end(), [](const Change& a, const Change& b) { return a.bytes > b.bytes; });
    std::string out;
    for (size_t i = 0; i < changes.size() && i < 10; ++i) {
        char one[96];
        snprintf(one, sizeof(one), "%s%s%s x %s%lld (%s%.2f MB)", out.empty() ? "" : ", ",
                 size_text(changes[i].size).c_str(), changes[i].size < 65536 ? "" : "+", before ? "+" : "",
                 static_cast<long long>(changes[i].blocks), before ? "+" : "", double(changes[i].bytes) / kMb);
        out += one;
    }
    return out.empty() ? "none" : out;
}

}  // namespace

bool install(const Config& cfg) {
    if (cfg.test_stress_pieces <= 0) return true;
    o_create_device = reinterpret_cast<CreateDeviceFn>(
        hooks::patch_import("d3d11.dll", "D3D11CreateDevice", reinterpret_cast<void*>(&d_create_device)));
    return o_create_device != nullptr;
}

void log(const char* when, uintptr_t havok_lo, uintptr_t havok_hi) {
    const int64_t t0 = session::qpc();

    // The heaps: their totals, and the busy blocks of the big or growing ones (at most 3).
    HANDLE heaps[64];
    const DWORD n_heaps = std::min<DWORD>(GetProcessHeaps(64, heaps), 64);
    static const auto summary =
        reinterpret_cast<HeapSummaryFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "HeapSummary"));
    struct HeapNow {
        HANDLE h;
        SIZE_T allocated, committed;
        double grown_mb, committed_grown_mb;
        int index;
    };
    std::vector<HeapNow> now;
    for (DWORD i = 0; i < n_heaps; ++i) {
        g_heap_of[reinterpret_cast<uintptr_t>(heaps[i])] = int(i);
        HEAP_SUMMARY s{};
        s.cb = sizeof(s);
        if (!summary || !summary(heaps[i], 0, &s)) continue;
        const auto last = g_heaps_last.find(heaps[i]);
        const bool known = last != g_heaps_last.end();
        now.push_back({heaps[i], s.cbAllocated, s.cbCommitted,
                       known ? (double(s.cbAllocated) - double(last->second.allocated)) / kMb : 0.0,
                       known ? (double(s.cbCommitted) - double(last->second.committed)) / kMb : 0.0, int(i)});
    }
    std::vector<HeapNow> to_walk;
    for (const HeapNow& h : now)
        if (h.allocated >= 32ull * 1048576 || h.grown_mb >= 4.0) to_walk.push_back(h);
    std::sort(to_walk.begin(), to_walk.end(), [](const HeapNow& a, const HeapNow& b) {
        return a.grown_mb != b.grown_mb ? a.grown_mb > b.grown_mb : a.allocated > b.allocated;
    });
    if (to_walk.size() > 3) to_walk.resize(3);
    std::vector<std::string> walked;
    for (const HeapNow& h : to_walk) {
        uint64_t entries = 0;
        const int64_t w0 = session::qpc();
        if (!walk(h.h, entries)) {
            walked.push_back("heap #" + std::to_string(h.index) + ": not walked (no lock of its own, or not an NT heap)");
            continue;
        }
        const double walk_ms = session::ticks_to_ms(session::qpc() - w0);
        for (size_t k = 0; k < g_region_count; ++k) g_heap_of[g_regions[k]] = h.index;
        HeapLast& last = g_heaps_last[h.h];
        const bool before = last.hist != nullptr;
        char head[224];
        snprintf(head, sizeof(head),
                 "heap #%d: %llu busy blocks, %.1f MB (walked in %.0f ms, %llu entries); by size, %s: ", h.index,
                 static_cast<unsigned long long>(g_walk.blocks), double(g_walk.bytes) / kMb, walk_ms,
                 static_cast<unsigned long long>(entries),
                 before ? "the most grown since the last census" : "the most bytes");
        walked.push_back(head + size_changes(g_walk, before ? last.hist.get() : nullptr));
        if (!last.hist) last.hist = std::make_unique<Histogram>();
        *last.hist = g_walk;
    }
    std::string heap_list;
    for (const HeapNow& h : now) {
        HeapLast& last = g_heaps_last[h.h];
        last.allocated = h.allocated;
        last.committed = h.committed;
        if (h.allocated < 8ull * 1048576 && std::fabs(h.grown_mb) < 0.5 && std::fabs(h.committed_grown_mb) < 0.5)
            continue;
        char one[160];
        snprintf(one, sizeof(one), "%s#%d%s %.1f MB allocated (%+.1f), %.1f committed (%+.1f)",
                 heap_list.empty() ? "" : "; ", h.index, h.h == GetProcessHeap() ? " (the process heap)" : "",
                 double(h.allocated) / kMb, h.grown_mb, double(h.committed) / kMb, h.committed_grown_mb);
        heap_list += one;
    }

    // Private memory by allocation, against the last census.
    const auto allocs = private_allocations();
    auto name_of = [&](uintptr_t base, const Alloc& a) -> std::string {
        const auto h = g_heap_of.find(base);
        if (h != g_heap_of.end()) return "heap #" + std::to_string(h->second);
        if (havok_lo && havok_lo >= base && havok_lo < base + a.size) return "Havok's heap";
        return "?";
    };
    uint64_t committed = 0;
    for (const auto& [base, a] : allocs) committed += a.committed;
    struct Grown {
        int64_t delta;
        uintptr_t base;
        Alloc a;
    };
    std::vector<Grown> grown;
    int64_t grown_total = 0, shrunk_total = 0;
    uint64_t new_count = 0, new_bytes = 0, gone_count = 0, gone_bytes = 0;
    std::unordered_map<uint64_t, std::pair<uint32_t, uint64_t>> new_by_size;  // reservation size -> count, committed
    for (const auto& [base, a] : allocs) {
        const auto it = g_allocs_last.find(base);
        if (it == g_allocs_last.end()) {
            if (!g_have_last) continue;
            ++new_count;
            new_bytes += a.committed;
            auto& g = new_by_size[a.size];
            ++g.first;
            g.second += a.committed;
            continue;
        }
        const int64_t d = int64_t(a.committed) - int64_t(it->second.committed);
        if (d > 0) {
            grown.push_back({d, base, a});
            grown_total += d;
        } else {
            shrunk_total += d;
        }
    }
    for (const auto& [base, a] : g_allocs_last)
        if (!allocs.count(base)) {
            ++gone_count;
            gone_bytes += a.committed;
        }
    std::sort(grown.begin(), grown.end(), [](const Grown& a, const Grown& b) { return a.delta > b.delta; });
    std::string grown_list;
    for (size_t i = 0; i < grown.size() && i < 8; ++i) {
        char one[128];
        snprintf(one, sizeof(one), "%s%08zX %+.2f MB (now %.1f of %.1f; %s)", grown_list.empty() ? "" : ", ",
                 size_t(grown[i].base), double(grown[i].delta) / kMb, double(grown[i].a.committed) / kMb,
                 double(grown[i].a.size) / kMb, name_of(grown[i].base, grown[i].a).c_str());
        grown_list += one;
    }
    std::vector<std::pair<uint64_t, std::pair<uint32_t, uint64_t>>> new_sizes(new_by_size.begin(), new_by_size.end());
    std::sort(new_sizes.begin(), new_sizes.end(),
              [](const auto& a, const auto& b) { return a.second.second > b.second.second; });
    std::string new_list;
    for (size_t i = 0; i < new_sizes.size() && i < 8; ++i) {
        char one[96];
        snprintf(one, sizeof(one), "%s%s x %u (%.2f MB committed)", new_list.empty() ? "" : ", ",
                 size_text(new_sizes[i].first).c_str(), new_sizes[i].second.first,
                 double(new_sizes[i].second.second) / kMb);
        new_list += one;
    }
    uint64_t last_committed = 0;
    for (const auto& [base, a] : g_allocs_last) last_committed += a.committed;

    LOG("memory %s: private memory committed %.1f MB in %zu allocations%s", when, double(committed) / kMb,
        allocs.size(), g_have_last ? "" : " (the first census)");
    if (g_have_last) {
        LOG("memory %s: %+.1f MB since the last census: %+.1f in %zu allocations that grew, %+.1f in %llu new, %.1f in "
            "those that shrank, %.1f in %llu gone", when, (double(committed) - double(last_committed)) / kMb,
            double(grown_total) / kMb, grown.size(), double(new_bytes) / kMb, static_cast<unsigned long long>(new_count),
            double(shrunk_total) / kMb, -double(gone_bytes) / kMb, static_cast<unsigned long long>(gone_count));
        LOG("memory %s: grew most: %s", when, grown_list.empty() ? "none" : grown_list.c_str());
        LOG("memory %s: new allocations by size: %s", when, new_list.empty() ? "none" : new_list.c_str());
    }
    LOG("memory %s: heaps (%lu): %s", when, static_cast<unsigned long>(n_heaps),
        heap_list.empty() ? "none of 8 MB or more" : heap_list.c_str());
    for (const std::string& w : walked) LOG("memory %s: %s", when, w.c_str());

    // D3D11.
    if (g_device_hooked) {
        std::string d3d;
        for (int k = 0; k < kKinds; ++k) {
            const int64_t bytes = g_live_bytes[k].load();
            const int32_t live = g_live[k].load();
            const uint32_t made = g_made[k].load();
            if (!live && !made) continue;
            char one[160];
            snprintf(one, sizeof(one), "%s%s %d (%+d), %.1f MB (%+.1f), %u made since the last census",
                     d3d.empty() ? "" : "; ", kKindNames[k], live, live - g_live_last[k], double(bytes) / kMb,
                     double(bytes - g_live_bytes_last[k]) / kMb, made - g_made_last[k]);
            d3d += one;
            g_live_bytes_last[k] = bytes;
            g_live_last[k] = live;
            g_made_last[k] = made;
        }
        LOG("memory %s: D3D11 alive: %s", when, d3d.empty() ? "nothing counted" : d3d.c_str());
    }
    g_allocs_last = allocs;
    g_have_last = true;
    LOG("memory %s: census took %.0f ms", when, session::ticks_to_ms(session::qpc() - t0));
}

}  // namespace memory_census
