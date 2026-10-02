// Xbox resources: formats, NV2A swizzling, textures, surfaces and buffers.
//
// Pixel and vertex data always live in title-visible memory (the Xbox model:
// the CPU writes resources directly). Host textures are caches of that memory,
// re-uploaded when its contents change. Everything except DXT is converted to
// BGRA8 on upload, since hosts lack most of the Xbox's 8/16-bit formats.
#include <algorithm>
#include <map>
#include <unordered_map>
#include <vector>

#include "d3d_internal.h"

namespace d3d {

namespace {

uint32_t g_frame = 1;

struct TextureEntry {
    gpu::Texture* tex = nullptr;
    uint32_t data = 0, format = 0, size = 0;
    uint64_t hash = 0;
    uint32_t checked_frame = 0;
    HostTarget* target = nullptr;  // set when the title renders into this texture
};
std::unordered_map<const xd3d::PixelContainer*, TextureEntry> g_textures;
std::unordered_map<const xd3d::Surface*, HostTarget*> g_targets;
std::map<std::pair<const xd3d::PixelContainer*, UINT>, xd3d::Surface*> g_levels;

uint64_t Hash(const uint8_t* p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t v;
        memcpy(&v, p + i, 8);
        h = (h ^ v) * 0x100000001b3ull;
        h ^= h >> 29;
    }
    for (; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

// NV2A swizzle: texel (x, y) lives at the bit-interleave of x and y, where
// interleaving stops for the shorter dimension.
void Unswizzle(const uint8_t* src, uint8_t* dst, UINT w, UINT h, UINT bytes) {
    uint32_t mx = 0, my = 0, bit = 1;
    for (UINT i = 1; i < w || i < h; i <<= 1) {
        if (i < w) mx |= bit, bit <<= 1;
        if (i < h) my |= bit, bit <<= 1;
    }
    uint32_t oy = 0;
    for (UINT y = 0; y < h; y++) {
        uint32_t ox = 0;
        for (UINT x = 0; x < w; x++) {
            memcpy(dst + (y * w + x) * bytes, src + (ox | oy) * bytes, bytes);
            ox = (ox - mx) & mx;
        }
        oy = (oy - my) & my;
    }
}

uint32_t Expand(uint32_t v, int bits) { return bits == 0 ? 0 : (v * 255 + ((1 << bits) - 1) / 2) / ((1 << bits) - 1); }
uint32_t Argb(uint32_t a, uint32_t r, uint32_t g, uint32_t b) { return a << 24 | r << 16 | g << 8 | b; }

// One texel of an Xbox format -> 0xAARRGGBB (B8G8R8A8 in memory).
uint32_t ToBgra(uint32_t fmt, const uint8_t* p) {
    using namespace xd3d;
    uint32_t v16 = p[0] | p[1] << 8;
    switch (fmt) {
        case FMT_L8: case FMT_LIN_L8: return Argb(255, p[0], p[0], p[0]);
        case FMT_AL8: case FMT_LIN_AL8: return Argb(p[0], p[0], p[0], p[0]);
        case FMT_A8: case FMT_LIN_A8: return Argb(p[0], 0, 0, 0);
        case FMT_A8L8: case FMT_LIN_A8L8: return Argb(p[1], p[0], p[0], p[0]);
        case FMT_L16: case FMT_LIN_L16: return Argb(255, p[1], p[1], p[1]);
        case FMT_A1R5G5B5: case FMT_LIN_A1R5G5B5:
            return Argb((v16 >> 15) ? 255 : 0, Expand((v16 >> 10) & 31, 5), Expand((v16 >> 5) & 31, 5), Expand(v16 & 31, 5));
        case FMT_X1R5G5B5: case FMT_LIN_X1R5G5B5:
            return Argb(255, Expand((v16 >> 10) & 31, 5), Expand((v16 >> 5) & 31, 5), Expand(v16 & 31, 5));
        case FMT_A4R4G4B4: case FMT_LIN_A4R4G4B4:
            return Argb(Expand(v16 >> 12, 4), Expand((v16 >> 8) & 15, 4), Expand((v16 >> 4) & 15, 4), Expand(v16 & 15, 4));
        case FMT_R5G6B5: case FMT_LIN_R5G6B5:
            return Argb(255, Expand(v16 >> 11, 5), Expand((v16 >> 5) & 63, 6), Expand(v16 & 31, 5));
        case FMT_A8R8G8B8: case FMT_LIN_A8R8G8B8: return p[0] | p[1] << 8 | p[2] << 16 | p[3] << 24;
        case FMT_X8R8G8B8: case FMT_LIN_X8R8G8B8: return p[0] | p[1] << 8 | p[2] << 16 | 0xFF000000u;
        case FMT_A8B8G8R8: case FMT_LIN_A8B8G8R8: return Argb(p[3], p[0], p[1], p[2]);
        case FMT_V8U8: return Argb(255, 0, p[1] ^ 0x80, p[0] ^ 0x80);
        default: return 0xFFFF00FF;  // magenta: unknown
    }
}

UINT LevelBytes(const FormatInfo& fi, UINT w, UINT h) {
    if (fi.compressed) return ((w + 3) / 4) * ((h + 3) / 4) * (fi.bpp * 2);  // 4 bpp -> 8-byte blocks
    return w * h * fi.bpp / 8;
}

UINT PitchOf(const xd3d::PixelContainer* p, const FormatInfo& fi, UINT w) {
    if (p->Size) return (((p->Size & xd3d::kSizePitchMask) >> xd3d::kSizePitchShift) + 1) * 64;
    return fi.compressed ? ((w + 3) / 4) * fi.bpp * 2 : w * fi.bpp / 8;
}

UINT TotalBytes(const xd3d::PixelContainer* p) {
    FormatInfo fi = GetFormatInfo(FormatOf(p));
    UINT w, h, levels;
    GetDimensions(p, &w, &h, &levels);
    if (p->Size) return PitchOf(p, fi, w) * h;
    UINT total = 0;
    for (UINT l = 0; l < levels; l++) total += LevelBytes(fi, std::max(w >> l, 1u), std::max(h >> l, 1u));
    if (p->Format & xd3d::kFormatCubemap) total = ((total + 127) & ~127u) * 6;
    return total;
}

UINT LevelOffset(const xd3d::PixelContainer* p, UINT level) {
    FormatInfo fi = GetFormatInfo(FormatOf(p));
    UINT w, h, levels, off = 0;
    GetDimensions(p, &w, &h, &levels);
    for (UINT l = 0; l < level; l++) off += LevelBytes(fi, std::max(w >> l, 1u), std::max(h >> l, 1u));
    return off;
}

// Decodes one mip level of title memory into host upload layout.
void DecodeLevel(const xd3d::PixelContainer* t, const FormatInfo& fi, uint32_t xfmt, const uint8_t* src, UINT w,
                 UINT h, std::vector<uint8_t>* out, UINT* row_pitch) {
    if (fi.compressed) {
        *row_pitch = ((w + 3) / 4) * fi.bpp * 2;
        out->assign(src, src + *row_pitch * ((h + 3) / 4));
        return;
    }
    UINT bytes = fi.bpp / 8;
    std::vector<uint8_t> linear;
    const uint8_t* lin = src;
    UINT pitch = PitchOf(t, fi, w);
    if (fi.swizzled) {
        linear.resize(size_t(w) * h * bytes);
        Unswizzle(src, linear.data(), w, h, bytes);
        lin = linear.data();
        pitch = w * bytes;
    }
    *row_pitch = w * 4;
    out->resize(size_t(w) * h * 4);
    auto* dst = reinterpret_cast<uint32_t*>(out->data());
    if (xfmt == xd3d::FMT_YUY2 || xfmt == xd3d::FMT_UYVY) {
        bool uyvy = xfmt == xd3d::FMT_UYVY;
        for (UINT y = 0; y < h; y++)
            for (UINT x = 0; x + 1 < w; x += 2) {
                const uint8_t* q = lin + y * pitch + x * 2;
                int y0 = uyvy ? q[1] : q[0], u = (uyvy ? q[0] : q[1]) - 128, y1 = uyvy ? q[3] : q[2],
                    v = (uyvy ? q[2] : q[3]) - 128;
                for (int k = 0; k < 2; k++) {
                    int c = (k ? y1 : y0) - 16;
                    auto clamp = [](int z) { return uint32_t(std::clamp(z, 0, 255)); };
                    dst[y * w + x + k] = Argb(255, clamp((298 * c + 409 * v + 128) >> 8),
                                              clamp((298 * c - 100 * u - 208 * v + 128) >> 8),
                                              clamp((298 * c + 516 * u + 128) >> 8));
                }
            }
        return;
    }
    for (UINT y = 0; y < h; y++)
        for (UINT x = 0; x < w; x++) dst[y * w + x] = ToBgra(xfmt, lin + y * pitch + x * bytes);
}

void Upload(TextureEntry& e, const xd3d::PixelContainer* t) {
    UINT w, h, levels;
    GetDimensions(t, &w, &h, &levels);
    uint32_t xfmt = FormatOf(t);
    FormatInfo fi = GetFormatInfo(xfmt);
    if (!fi.supported) {
        static int logged = 0;
        if (logged++ < 10) Log("texture %p: unsupported format 0x%02x (%ux%u)", t, xfmt, w, h);
        return;
    }
    bool cube = (t->Format & xd3d::kFormatCubemap) != 0;
    if (fi.compressed && ((w & 3) || (h & 3))) levels = 1;  // BCn needs block-aligned top levels
    UINT faces = cube ? 6 : 1;
    std::vector<std::vector<uint8_t>> storage(faces * levels);
    std::vector<gpu::SubresourceData> init(faces * levels);
    const uint8_t* src = G2H<const uint8_t>(t->Data);
    UINT face_bytes = cube ? TotalBytes(t) / 6 : 0;
    for (UINT f = 0; f < faces; f++) {
        const uint8_t* p = src + f * face_bytes;
        for (UINT l = 0; l < levels; l++) {
            UINT lw = std::max(w >> l, 1u), lh = std::max(h >> l, 1u), pitch;
            DecodeLevel(t, fi, xfmt, p, lw, lh, &storage[f * levels + l], &pitch);
            init[f * levels + l] = {storage[f * levels + l].data(), pitch};
            p += LevelBytes(fi, lw, lh);
        }
    }
    gpu::DestroyTexture(e.tex);
    e.tex = gpu::CreateTexture(w, h, levels, fi.host, cube, init.data());
    if (!e.tex) {
        Log("texture %p (%ux%u x%u fmt 0x%02x): host texture creation failed", t, w, h, levels, xfmt);
        return;
    }
    e.format = t->Format, e.size = t->Size;
}

}  // namespace

FormatInfo GetFormatInfo(uint32_t f) {
    using namespace xd3d;
    constexpr gpu::Format kBgra = gpu::Format::BGRA8, kDepth = gpu::Format::D24S8;
    switch (f) {
        case FMT_L8: case FMT_AL8: case FMT_A8: return {8, true, false, false, kBgra, true};
        case FMT_LIN_L8: case FMT_LIN_AL8: case FMT_LIN_A8: return {8, false, false, false, kBgra, true};
        case FMT_A8L8: case FMT_A1R5G5B5: case FMT_X1R5G5B5: case FMT_A4R4G4B4: case FMT_R5G6B5: case FMT_V8U8:
        case FMT_L16:
            return {16, true, false, false, kBgra, true};
        case FMT_LIN_A8L8: case FMT_LIN_A1R5G5B5: case FMT_LIN_X1R5G5B5: case FMT_LIN_A4R4G4B4: case FMT_LIN_R5G6B5:
        case FMT_LIN_L16: case FMT_YUY2: case FMT_UYVY:
            return {16, false, false, false, kBgra, true};
        case FMT_A8R8G8B8: case FMT_X8R8G8B8: case FMT_A8B8G8R8: return {32, true, false, false, kBgra, true};
        case FMT_LIN_A8R8G8B8: case FMT_LIN_X8R8G8B8: case FMT_LIN_A8B8G8R8:
            return {32, false, false, false, kBgra, true};
        case FMT_DXT1: return {4, false, true, false, gpu::Format::BC1, true};
        case FMT_DXT3: return {8, false, true, false, gpu::Format::BC2, true};
        case FMT_DXT5: return {8, false, true, false, gpu::Format::BC3, true};
        case FMT_D24S8: case FMT_F24S8: return {32, true, false, true, kDepth, false};
        case FMT_LIN_D24S8: case FMT_LIN_F24S8: return {32, false, false, true, kDepth, false};
        case FMT_D16: case FMT_F16: return {16, true, false, true, kDepth, false};
        case FMT_LIN_D16: case FMT_LIN_F16: return {16, false, false, true, kDepth, false};
        default: return {32, false, false, false, kBgra, false};
    }
}

uint32_t FormatOf(const xd3d::PixelContainer* p) {
    return (p->Format & xd3d::kFormatFormatMask) >> xd3d::kFormatFormatShift;
}

void GetDimensions(const xd3d::PixelContainer* p, UINT* w, UINT* h, UINT* levels) {
    if (p->Size) {
        *w = (p->Size & xd3d::kSizeWidthMask) + 1;
        *h = ((p->Size & xd3d::kSizeHeightMask) >> xd3d::kSizeHeightShift) + 1;
        *levels = 1;
    } else {
        *w = 1u << ((p->Format >> xd3d::kFormatUSizeShift) & 0xF);
        *h = 1u << ((p->Format >> xd3d::kFormatVSizeShift) & 0xF);
        *levels = std::max(1u, (p->Format & xd3d::kFormatMipmapMask) >> xd3d::kFormatMipmapShift);
    }
}

xd3d::Surface* NewSurfaceHeader(uint32_t format, UINT w, UINT h, uint32_t data) {
    auto* s = static_cast<xd3d::Surface*>(GuestAlloc(sizeof(xd3d::Surface), PAGE_READWRITE, 16));
    s->Common = 1 | xd3d::kCommonTypeSurface | xd3d::kCommonD3DCreated;
    s->Data = data;
    s->Format = (format << xd3d::kFormatFormatShift) | (2 << xd3d::kFormatDimensionShift) | 1;
    FormatInfo fi = GetFormatInfo(format);
    UINT pitch = (w * fi.bpp / 8 + 63) & ~63u;
    s->Size = (w - 1) | ((h - 1) << xd3d::kSizeHeightShift) | ((pitch / 64 - 1) << xd3d::kSizePitchShift);
    return s;
}

void RegisterTarget(xd3d::Surface* s, HostTarget* t) { g_targets[s] = t; }

void ResourcesBeginFrame() { g_frame++; }

gpu::Texture* HostTexture(xd3d::PixelContainer* t) {
    if (!t) return nullptr;
    if ((t->Common & xd3d::kCommonTypeMask) == xd3d::kCommonTypeSurface) {
        // A surface bound as a texture (e.g. a render target): use its target.
        auto it = g_targets.find(static_cast<xd3d::Surface*>(t));
        if (it != g_targets.end()) return it->second->tex;
    }
    TextureEntry& e = g_textures[t];
    if (e.target) return e.target->tex;
    if (e.checked_frame == g_frame && e.data == t->Data && e.format == t->Format) return e.tex;
    e.checked_frame = g_frame;
    if (!t->Data) return nullptr;
    uint64_t h = Hash(G2H<const uint8_t>(t->Data), TotalBytes(t));
    if (!e.tex || h != e.hash || e.data != t->Data || e.format != t->Format || e.size != t->Size) {
        Upload(e, t);
        e.hash = h;
        e.data = t->Data;
    }
    return e.tex;
}

HostTarget* HostTargetFor(xd3d::Surface* s) {
    auto it = g_targets.find(s);
    if (it != g_targets.end()) return it->second;
    UINT w, h, l;
    GetDimensions(s, &w, &h, &l);
    FormatInfo fi = GetFormatInfo(FormatOf(s));
    auto* t = new HostTarget;
    if (s->Parent) {
        // Rendering into a texture level: the whole texture becomes a host
        // render target from now on (its memory contents are no longer used).
        TextureEntry& e = g_textures[s->Parent.get()];
        if (!e.target) {
            UINT pw, ph, pl;
            GetDimensions(s->Parent.get(), &pw, &ph, &pl);
            e.target = t;
            CreateTarget(t, pw, ph, fi.depth);
            Log("texture %08x (%ux%u fmt 0x%02x) is now a render target", s->Parent.addr, pw, ph,
                FormatOf(s->Parent.get()));
        } else {
            delete t;
            t = e.target;
        }
    } else {
        CreateTarget(t, w, h, fi.depth);
    }
    g_targets[s] = t;
    return t;
}

// Existing host target for a surface (registered, or its texture renders), or null.
static HostTarget* ExistingTarget(xd3d::Surface* s) {
    auto it = g_targets.find(s);
    if (it != g_targets.end()) return it->second;
    if (s->Parent) {
        auto t = g_textures.find(s->Parent.get());
        if (t != g_textures.end() && t->second.target) return t->second.target;
    }
    return nullptr;
}

static void SwizzleInto(const uint8_t* src, uint8_t* dst, UINT w, UINT h, UINT bytes) {
    uint32_t mx = 0, my = 0, bit = 1;
    for (UINT i = 1; i < w || i < h; i <<= 1) {
        if (i < w) mx |= bit, bit <<= 1;
        if (i < h) my |= bit, bit <<= 1;
    }
    uint32_t oy = 0;
    for (UINT y = 0; y < h; y++) {
        uint32_t ox = 0;
        for (UINT x = 0; x < w; x++) {
            memcpy(dst + (ox | oy) * bytes, src + (y * w + x) * bytes, bytes);
            ox = (ox - mx) & mx;
        }
        oy = (oy - my) & my;
    }
}

// ---- Exports ---------------------------------------------------------------------------

ULONG WINAPI x_D3DResource_AddRef(xd3d::Resource* r) { return ++r->Common & xd3d::kCommonRefcountMask; }

ULONG WINAPI x_D3DResource_Release(xd3d::Resource* r) {
    // Host objects stay cached; memory is never freed (acceptable for one session).
    if ((r->Common & xd3d::kCommonRefcountMask) == 0) return 0;
    return --r->Common & xd3d::kCommonRefcountMask;
}

// Resources loaded by the title store Data relative to the block they came in.
void WINAPI x_D3DResource_Register(xd3d::Resource* r, uint32_t base) { r->Data += base; }

void WINAPI x_D3DResource_BlockUntilNotBusy(xd3d::Resource*) {}

DWORD WINAPI x_D3DResource_GetType(xd3d::Resource* r) {
    switch (r->Common & xd3d::kCommonTypeMask) {
        case xd3d::kCommonTypeVertexBuffer: return 6;  // D3DRTYPE_VERTEXBUFFER
        case xd3d::kCommonTypeIndexBuffer: return 7;
        case xd3d::kCommonTypeSurface: return 1;
        case xd3d::kCommonTypeTexture:
            return (static_cast<xd3d::PixelContainer*>(r)->Format & xd3d::kFormatCubemap) ? 5 : 3;
        default: return 0;
    }
}

xd3d::PixelContainer* WINAPI x_D3DDevice_CreateTexture2(UINT w, UINT h, UINT depth, UINT levels, DWORD usage,
                                                        DWORD format, DWORD type) {
    D3D_LOCK;
    auto* t = static_cast<xd3d::PixelContainer*>(GuestAlloc(sizeof(xd3d::PixelContainer), PAGE_READWRITE, 16));
    t->Common = 1 | xd3d::kCommonTypeTexture | xd3d::kCommonD3DCreated;
    FormatInfo fi = GetFormatInfo(format);
    UINT log_w = 0, log_h = 0;
    while ((1u << log_w) < w) log_w++;
    while ((1u << log_h) < h) log_h++;
    if (levels == 0) levels = std::max(log_w, log_h) + 1;
    t->Format = (format << xd3d::kFormatFormatShift) | (2 << xd3d::kFormatDimensionShift) | 1;
    if (type == 5) t->Format |= xd3d::kFormatCubemap;  // D3DRTYPE_CUBETEXTURE
    if (fi.swizzled || fi.compressed) {
        t->Format |= (levels << xd3d::kFormatMipmapShift) | (log_w << xd3d::kFormatUSizeShift) |
                     (log_h << xd3d::kFormatVSizeShift);
    } else {
        UINT pitch = (w * fi.bpp / 8 + 63) & ~63u;
        t->Format |= 1 << xd3d::kFormatMipmapShift;
        t->Size = (w - 1) | ((h - 1) << xd3d::kSizeHeightShift) | ((pitch / 64 - 1) << xd3d::kSizePitchShift);
    }
    t->Data = H2G(GuestAlloc(TotalBytes(t)));
    Log("CreateTexture2(%ux%u, levels %u, usage %lx, fmt 0x%02lx, type %lu) -> %08x", w, h, levels, usage, format,
        type, H2G(t));
    return t;
}

// Surface for one face/level; cached so the pointer is stable.
xd3d::Surface* LevelSurface(xd3d::PixelContainer* t, UINT face, UINT level) {
    xd3d::Surface*& s = g_levels[{t, face * 16 + level}];
    if (!s) {
        UINT w, h, levels;
        GetDimensions(t, &w, &h, &levels);
        w = std::max(w >> level, 1u), h = std::max(h >> level, 1u);
        UINT face_off = (t->Format & xd3d::kFormatCubemap) ? face * (TotalBytes(t) / 6) : 0;
        s = NewSurfaceHeader(FormatOf(t), w, h, t->Data + face_off + LevelOffset(t, level));
        if (!t->Size) {
            UINT lw = 0, lh = 0;
            while ((1u << lw) < w) lw++;
            while ((1u << lh) < h) lh++;
            s->Format = (t->Format & 0x0000FFFBu) | (1 << xd3d::kFormatMipmapShift) | (lw << xd3d::kFormatUSizeShift) |
                        (lh << xd3d::kFormatVSizeShift);
            s->Size = 0;
        }
        s->Parent = t;
    }
    s->Common++;
    return s;
}

xd3d::Surface* WINAPI x_D3DTexture_GetSurfaceLevel2(xd3d::PixelContainer* t, UINT level) {
    D3D_LOCK;
    return LevelSurface(t, 0, level);
}

xd3d::Surface* WINAPI x_D3DCubeTexture_GetCubeMapSurface2(xd3d::PixelContainer* t, DWORD face, UINT level) {
    D3D_LOCK;
    return LevelSurface(t, face, level);
}

struct XSurfaceDesc {
    DWORD Format, Type, Usage, Size, MultiSampleType, Width, Height;
};

// Internal helper behind the inline D3DTexture_GetLevelDesc.
void WINAPI x_Get2DSurfaceDesc(xd3d::PixelContainer* p, DWORD level, XSurfaceDesc* d) {
    UINT w, h, l;
    GetDimensions(p, &w, &h, &l);
    w = std::max(w >> level, 1u), h = std::max(h >> level, 1u);
    FormatInfo fi = GetFormatInfo(FormatOf(p));
    *d = {FormatOf(p), 1, 0, p->Size ? PitchOf(p, fi, w) * h : LevelBytes(fi, w, h), 0, w, h};
}

UINT WINAPI x_D3DBaseTexture_GetLevelCount(xd3d::PixelContainer* p) {
    UINT w, h, l;
    GetDimensions(p, &w, &h, &l);
    return l;
}

HRESULT WINAPI x_D3DSurface_GetDesc(xd3d::PixelContainer* s, XSurfaceDesc* d) {
    UINT w, h, l;
    GetDimensions(s, &w, &h, &l);
    *d = {FormatOf(s), 1, 0, TotalBytes(s), 0, w, h};
    return S_OK;
}

struct D3DLOCKED_RECT_X {
    INT Pitch;
    uint32_t pBits;  // guest address
};

HRESULT WINAPI x_D3DTexture_LockRect(xd3d::PixelContainer* t, UINT level, D3DLOCKED_RECT_X* lr, const RECT* rect,
                                     DWORD) {
    UINT w, h, l;
    GetDimensions(t, &w, &h, &l);
    FormatInfo fi = GetFormatInfo(FormatOf(t));
    lr->Pitch = PitchOf(t, fi, std::max(w >> level, 1u));
    auto* p = G2H<uint8_t>(t->Data) + LevelOffset(t, level);
    if (rect) p += rect->top * lr->Pitch + rect->left * fi.bpp / 8;
    lr->pBits = H2G(p);
    return S_OK;
}

// Internal helper behind the inline texture/cube LockRect variants.
HRESULT WINAPI x_Lock2DSurface(xd3d::PixelContainer* t, DWORD face, UINT level, D3DLOCKED_RECT_X* lr, const RECT* rect,
                               DWORD) {
    UINT w, h, l;
    GetDimensions(t, &w, &h, &l);
    FormatInfo fi = GetFormatInfo(FormatOf(t));
    lr->Pitch = PitchOf(t, fi, std::max(w >> level, 1u));
    UINT face_off = (t->Format & xd3d::kFormatCubemap) ? face * (TotalBytes(t) / 6) : 0;
    auto* p = G2H<uint8_t>(t->Data) + face_off + LevelOffset(t, level);
    if (rect) p += rect->top * lr->Pitch + rect->left * fi.bpp / 8;
    lr->pBits = H2G(p);
    return S_OK;
}

HRESULT WINAPI x_D3DSurface_LockRect(xd3d::Surface* s, D3DLOCKED_RECT_X* lr, const RECT* rect, DWORD) {
    UINT w, h, l;
    GetDimensions(s, &w, &h, &l);
    FormatInfo fi = GetFormatInfo(FormatOf(s));
    lr->Pitch = PitchOf(s, fi, w);
    if (!s->Data) {
        s->Data = H2G(GuestAlloc(lr->Pitch * h));
        Log("D3DSurface_LockRect: surface %p has no backing memory; render results are not read back", s);
    }
    auto* p = G2H<uint8_t>(s->Data);
    if (rect) p += rect->top * lr->Pitch + rect->left * fi.bpp / 8;
    lr->pBits = H2G(p);
    return S_OK;
}

// Copies rectangles between surfaces. Render targets live on the GPU, other
// surfaces in title memory; both directions are supported (32-bit formats
// for memory destinations).
HRESULT WINAPI x_D3DDevice_CopyRects(xd3d::Surface* src, const RECT* rects, UINT count, xd3d::Surface* dst,
                                     const POINT* points) {
    D3D_LOCK;
    UINT sw, sh, dw, dh, l;
    GetDimensions(src, &sw, &sh, &l);
    GetDimensions(dst, &dw, &dh, &l);
    RECT whole{0, 0, LONG(sw), LONG(sh)};
    if (!count || !rects) count = 1, rects = &whole;
    HostTarget* hs = ExistingTarget(src);
    HostTarget* hd = ExistingTarget(dst);
    FormatInfo dfi = GetFormatInfo(FormatOf(dst));
    for (UINT i = 0; i < count; i++) {
        RECT r = rects[i];
        LONG dx = points ? points[i].x : r.left, dy = points ? points[i].y : r.top;
        r.right = std::min<LONG>(r.right, r.left + LONG(dw) - dx);
        r.bottom = std::min<LONG>(r.bottom, r.top + LONG(dh) - dy);
        if (r.right <= r.left || r.bottom <= r.top) continue;
        UINT w = r.right - r.left, h = r.bottom - r.top;
        gpu::Rect gr{r.left, r.top, r.right, r.bottom};
        if (hs && hd) {
            gpu::CopyTexture(hd->tex, dx, dy, hs->tex, gr);
        } else if (hs && !hd) {
            // GPU -> title memory (e.g. a screenshot of the back buffer).
            if (dfi.bpp != 32 || !dst->Data) continue;
            std::vector<uint8_t> px;
            if (!gpu::ReadTexture(hs->tex, gr, &px)) continue;
            auto* out = G2H<uint8_t>(dst->Data);
            if (dfi.swizzled) {
                // Swizzled destinations are rewritten whole: unswizzle, patch, reswizzle.
                std::vector<uint8_t> lin(size_t(dw) * dh * 4);
                Unswizzle(out, lin.data(), dw, dh, 4);
                for (UINT y = 0; y < h; y++) memcpy(&lin[((dy + y) * dw + dx) * 4], px.data() + size_t(y) * w * 4, w * 4);
                SwizzleInto(lin.data(), out, dw, dh, 4);
            } else {
                UINT pitch = PitchOf(dst, dfi, dw);
                for (UINT y = 0; y < h; y++) memcpy(out + (dy + y) * pitch + dx * 4, px.data() + size_t(y) * w * 4, w * 4);
            }
        } else if (!hs && hd) {
            // Title memory -> GPU target.
            std::vector<uint8_t> pixels;
            UINT pitch;
            FormatInfo sfi = GetFormatInfo(FormatOf(src));
            DecodeLevel(src, sfi, FormatOf(src), G2H<const uint8_t>(src->Data), sw, sh,
                        &pixels, &pitch);
            gpu::UpdateTexture(hd->tex, {dx, dy, dx + LONG(w), dy + LONG(h)}, pixels.data() + r.top * pitch + r.left * 4,
                               pitch);
        } else {
            FormatInfo sfi = GetFormatInfo(FormatOf(src));
            if (sfi.swizzled || dfi.swizzled || sfi.bpp != dfi.bpp || !src->Data || !dst->Data) continue;
            UINT bpp = sfi.bpp / 8, sp = PitchOf(src, sfi, sw), dp = PitchOf(dst, dfi, dw);
            for (UINT y = 0; y < h; y++)
                memcpy(G2H<uint8_t>(dst->Data) + (dy + y) * dp + dx * bpp,
                       G2H<uint8_t>(src->Data) + (r.top + y) * sp + r.left * bpp, w * bpp);
        }
    }
    return S_OK;
}

// ---- Vertex / index buffers: plain title memory ---------------------------------------------

xd3d::Resource* NewBuffer(UINT length, uint32_t type) {
    auto* b = static_cast<xd3d::Resource*>(GuestAlloc(sizeof(xd3d::Resource), PAGE_READWRITE, 16));
    b->Common = 1 | type | xd3d::kCommonD3DCreated;
    b->Data = H2G(GuestAlloc(length));
    return b;
}

xd3d::Resource* WINAPI x_D3DDevice_CreateVertexBuffer2(UINT length) {
    return NewBuffer(length, xd3d::kCommonTypeVertexBuffer);
}
xd3d::Resource* WINAPI x_D3DDevice_CreateIndexBuffer2(UINT length) {
    return NewBuffer(length, xd3d::kCommonTypeIndexBuffer);
}

BYTE* WINAPI x_D3DVertexBuffer_Lock2(xd3d::Resource* vb, DWORD) { return G2H<BYTE>(vb->Data); }

struct XVertexBufferDesc {
    DWORD Format, Type, Usage, Pool, Size, FVF;
};

HRESULT WINAPI x_D3DVertexBuffer_GetDesc(xd3d::Resource* vb, XVertexBufferDesc* d) {
    // Size: the rest of the committed region holding the data.
    GuestRegion r{};
    GuestQuery(vb->Data, &r);
    *d = {xd3d::FMT_VERTEXDATA, 6, 0, 0, DWORD(r.size - (vb->Data - r.base)), 0};
    return S_OK;
}

void* WINAPI x_D3D_AllocContiguousMemory(uint32_t size, DWORD align) {
    return GuestAlloc(size, PAGE_READWRITE, align > 0x1000 ? align : 0x1000);
}

HLE_EXPORT("D3D8", D3DResource_AddRef);
HLE_EXPORT("D3D8", D3DResource_Release);
HLE_EXPORT("D3D8", D3DResource_Register);
HLE_EXPORT("D3D8", D3DResource_BlockUntilNotBusy);
HLE_EXPORT("D3D8", D3DResource_GetType);
HLE_EXPORT("D3D8", D3DDevice_CreateTexture2);
HLE_EXPORT("D3D8", D3DDevice_CopyRects);
HLE_EXPORT("D3D8", D3DTexture_GetSurfaceLevel2);
HLE_EXPORT("D3D8", D3DCubeTexture_GetCubeMapSurface2);
HLE_EXPORT("D3D8", Lock2DSurface);
HLE_EXPORT("D3D8", D3DSurface_GetDesc);
HLE_EXPORT("D3D8", Get2DSurfaceDesc);
HLE_EXPORT("D3D8", D3DBaseTexture_GetLevelCount);
HLE_EXPORT("D3D8", D3DTexture_LockRect);
HLE_EXPORT("D3D8", D3DSurface_LockRect);
HLE_EXPORT("D3D8", D3DDevice_CreateVertexBuffer2);
HLE_EXPORT("D3D8", D3DDevice_CreateIndexBuffer2);
HLE_EXPORT("D3D8", D3DVertexBuffer_Lock2);
HLE_EXPORT("D3D8", D3DVertexBuffer_GetDesc);
HLE_EXPORT("D3D8", D3D_AllocContiguousMemory);

}  // namespace d3d
