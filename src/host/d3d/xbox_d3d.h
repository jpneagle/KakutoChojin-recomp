// Xbox D3D8 (XDK 4xxx) structures and enumerations as seen by title code.
#pragma once

#include <cstdint>

#include "../guest.h"

namespace xd3d {

// ---- Resources -----------------------------------------------------------------
// Title code allocates and reads these headers directly.

struct Resource {
    uint32_t Common;  // refcount (bits 0-15), type (bits 16-18), flags
    uint32_t Data;    // pixel/vertex data (guest address)
    uint32_t Lock;
};
struct PixelContainer : Resource {
    uint32_t Format;  // dimensionality, format, mip count, log2 sizes
    uint32_t Size;    // linear formats: width-1 | height-1 << 12 | pitch/64-1 << 24
};
struct Surface : PixelContainer {
    GPtr<PixelContainer> Parent;
};
static_assert(sizeof(Surface) == 24, "D3DSurface layout");

constexpr uint32_t kCommonRefcountMask = 0x0000FFFF;
constexpr uint32_t kCommonTypeMask = 0x00070000;
constexpr uint32_t kCommonTypeVertexBuffer = 0x00000000;
constexpr uint32_t kCommonTypeIndexBuffer = 0x00010000;
constexpr uint32_t kCommonTypePushBuffer = 0x00020000;
constexpr uint32_t kCommonTypePalette = 0x00030000;
constexpr uint32_t kCommonTypeTexture = 0x00040000;
constexpr uint32_t kCommonTypeSurface = 0x00050000;
constexpr uint32_t kCommonD3DCreated = 0x01000000;

// PixelContainer::Format bit fields
constexpr uint32_t kFormatCubemap = 0x00000004;
constexpr uint32_t kFormatDimensionShift = 4, kFormatDimensionMask = 0x000000F0;
constexpr uint32_t kFormatFormatShift = 8, kFormatFormatMask = 0x0000FF00;
constexpr uint32_t kFormatMipmapShift = 16, kFormatMipmapMask = 0x000F0000;
constexpr uint32_t kFormatUSizeShift = 20, kFormatVSizeShift = 24, kFormatPSizeShift = 28;

// Size bit fields (linear formats)
constexpr uint32_t kSizeWidthMask = 0x00000FFF, kSizeHeightMask = 0x00FFF000, kSizeHeightShift = 12;
constexpr uint32_t kSizePitchMask = 0xFF000000, kSizePitchShift = 24;

// D3DFORMAT values (texture formats are NV2A color formats).
enum Format : uint32_t {
    FMT_L8 = 0x00, FMT_AL8 = 0x01, FMT_A1R5G5B5 = 0x02, FMT_X1R5G5B5 = 0x03, FMT_A4R4G4B4 = 0x04,
    FMT_R5G6B5 = 0x05, FMT_A8R8G8B8 = 0x06, FMT_X8R8G8B8 = 0x07, FMT_P8 = 0x0B, FMT_DXT1 = 0x0C,
    FMT_DXT3 = 0x0E, FMT_DXT5 = 0x0F, FMT_LIN_A1R5G5B5 = 0x10, FMT_LIN_R5G6B5 = 0x11,
    FMT_LIN_A8R8G8B8 = 0x12, FMT_LIN_L8 = 0x13, FMT_R8B8 = 0x16, FMT_G8B8 = 0x17, FMT_A8 = 0x19,
    FMT_A8L8 = 0x1A, FMT_LIN_AL8 = 0x1B, FMT_LIN_X1R5G5B5 = 0x1C, FMT_LIN_A4R4G4B4 = 0x1D,
    FMT_LIN_X8R8G8B8 = 0x1E, FMT_LIN_A8 = 0x1F, FMT_LIN_A8L8 = 0x20, FMT_YUY2 = 0x24, FMT_UYVY = 0x25,
    FMT_L6V5U5 = 0x27, FMT_V8U8 = 0x28, FMT_D24S8 = 0x2A, FMT_F24S8 = 0x2B, FMT_D16 = 0x2C,
    FMT_F16 = 0x2D, FMT_LIN_D24S8 = 0x2E, FMT_LIN_F24S8 = 0x2F, FMT_LIN_D16 = 0x30, FMT_LIN_F16 = 0x31,
    FMT_L16 = 0x32, FMT_V16U16 = 0x33, FMT_LIN_L16 = 0x35, FMT_LIN_V16U16 = 0x36, FMT_LIN_L6V5U5 = 0x37,
    FMT_R6G5B5 = 0x38, FMT_A8B8G8R8 = 0x3A, FMT_B8G8R8A8 = 0x3B, FMT_R8G8B8A8 = 0x3C,
    FMT_LIN_A8B8G8R8 = 0x3F, FMT_LIN_B8G8R8A8 = 0x40, FMT_LIN_R8G8B8A8 = 0x41,
    FMT_VERTEXDATA = 100, FMT_INDEX16 = 101,
};

// ---- Presentation -----------------------------------------------------------------

struct PresentParameters {
    uint32_t BackBufferWidth, BackBufferHeight, BackBufferFormat, BackBufferCount;
    uint32_t MultiSampleType, SwapEffect, hDeviceWindow, Windowed;
    uint32_t EnableAutoDepthStencil, AutoDepthStencilFormat, Flags;
    uint32_t FullScreen_RefreshRateInHz, FullScreen_PresentationInterval;
    GPtr<Surface> BufferSurfaces[3];
    GPtr<Surface> DepthStencilSurface;
};

struct Rect {
    int32_t x1, y1, x2, y2;
};

// D3DCLEAR flags differ from the PC: per-channel target bits.
constexpr uint32_t kClearZBuffer = 0x01, kClearStencil = 0x02, kClearTargetMask = 0xF0;

// ---- Render state indices (XDK 4928 layout, verified against XbSymbolDatabase) ---------

enum RenderState : uint32_t {
    RS_PS_FIRST = 0,  // 0..56: pixel shader (register combiner) state
    RS_ZFUNC = 57, RS_ALPHAFUNC, RS_ALPHABLENDENABLE, RS_ALPHATESTENABLE, RS_ALPHAREF, RS_SRCBLEND,
    RS_DESTBLEND, RS_ZWRITEENABLE, RS_DITHERENABLE, RS_SHADEMODE, RS_COLORWRITEENABLE, RS_STENCILZFAIL,
    RS_STENCILPASS, RS_STENCILFUNC, RS_STENCILREF, RS_STENCILMASK, RS_STENCILWRITEMASK, RS_BLENDOP,
    RS_BLENDCOLOR, RS_SWATHWIDTH, RS_POLYGONOFFSETZSLOPESCALE, RS_POLYGONOFFSETZOFFSET,
    RS_POINTOFFSETENABLE, RS_WIREFRAMEOFFSETENABLE, RS_SOLIDOFFSETENABLE, RS_DEPTHCLIPCONTROL,
    RS_STIPPLEENABLE,
    RS_FOGENABLE = 92, RS_FOGTABLEMODE, RS_FOGSTART, RS_FOGEND, RS_FOGDENSITY, RS_RANGEFOGENABLE,
    RS_WRAP0, RS_WRAP1, RS_WRAP2, RS_WRAP3, RS_LIGHTING, RS_SPECULARENABLE, RS_LOCALVIEWER,
    RS_COLORVERTEX, RS_BACKSPECULARMATERIALSOURCE, RS_BACKDIFFUSEMATERIALSOURCE,
    RS_BACKAMBIENTMATERIALSOURCE, RS_BACKEMISSIVEMATERIALSOURCE, RS_SPECULARMATERIALSOURCE,
    RS_DIFFUSEMATERIALSOURCE, RS_AMBIENTMATERIALSOURCE, RS_EMISSIVEMATERIALSOURCE, RS_BACKAMBIENT,
    RS_AMBIENT, RS_POINTSIZE, RS_POINTSIZE_MIN, RS_POINTSPRITEENABLE, RS_POINTSCALEENABLE,
    RS_POINTSCALE_A, RS_POINTSCALE_B, RS_POINTSCALE_C, RS_POINTSIZE_MAX, RS_PATCHEDGESTYLE,
    RS_PATCHSEGMENTS, RS_SWAPFILTER, RS_PRESENTATIONINTERVAL,
    RS_PSTEXTUREMODES = 136, RS_VERTEXBLEND, RS_FOGCOLOR, RS_FILLMODE, RS_BACKFILLMODE,
    RS_TWOSIDEDLIGHTING, RS_NORMALIZENORMALS, RS_ZENABLE, RS_STENCILENABLE, RS_STENCILFAIL,
    RS_FRONTFACE, RS_CULLMODE, RS_TEXTUREFACTOR, RS_ZBIAS, RS_LOGICOP, RS_EDGEANTIALIAS,
    RS_MULTISAMPLEANTIALIAS, RS_MULTISAMPLEMASK, RS_MULTISAMPLEMODE, RS_MULTISAMPLERENDERTARGETMODE,
    RS_SHADOWFUNC, RS_LINEWIDTH, RS_SAMPLEALPHA, RS_DXT1NOISEENABLE, RS_YUVENABLE,
    RS_OCCLUSIONCULLENABLE, RS_STENCILCULLENABLE, RS_ROPZCMPALWAYSREAD, RS_ROPZREAD,
    RS_DONOTCULLUNCOMPRESSED,
    RS_MAX
};

// Texture stage state indices; the title stores them at DeferredTextureState[stage][32].
enum TextureStageState : uint32_t {
    TSS_ADDRESSU = 0, TSS_ADDRESSV, TSS_ADDRESSW, TSS_MAGFILTER, TSS_MINFILTER, TSS_MIPFILTER,
    TSS_MIPMAPLODBIAS, TSS_MAXMIPLEVEL, TSS_MAXANISOTROPY, TSS_COLORKEYOP, TSS_COLORSIGN, TSS_ALPHAKILL,
    TSS_COLOROP, TSS_COLORARG0, TSS_COLORARG1, TSS_COLORARG2, TSS_ALPHAOP, TSS_ALPHAARG0, TSS_ALPHAARG1,
    TSS_ALPHAARG2, TSS_RESULTARG, TSS_TEXTURETRANSFORMFLAGS,
    TSS_BUMPENVMAT00 = 22, TSS_BUMPENVMAT01, TSS_BUMPENVMAT11, TSS_BUMPENVMAT10, TSS_BUMPENVLSCALE,
    TSS_BUMPENVLOFFSET, TSS_TEXCOORDINDEX, TSS_BORDERCOLOR, TSS_COLORKEYCOLOR,
    TSS_PER_STAGE = 32
};

// Primitive types (NV2A begin/end values).
enum Primitive : uint32_t {
    PT_POINTLIST = 1, PT_LINELIST, PT_LINELOOP, PT_LINESTRIP, PT_TRIANGLELIST, PT_TRIANGLESTRIP,
    PT_TRIANGLEFAN, PT_QUADLIST, PT_QUADSTRIP, PT_POLYGON
};

// D3DTRANSFORMSTATETYPE: Xbox uses a compact numbering.
enum Transform : uint32_t {
    TS_VIEW = 0, TS_PROJECTION = 1, TS_TEXTURE0 = 2, TS_TEXTURE1, TS_TEXTURE2, TS_TEXTURE3,
    TS_WORLD = 6, TS_WORLD1, TS_WORLD2, TS_WORLD3, TS_MAX
};

}  // namespace xd3d
