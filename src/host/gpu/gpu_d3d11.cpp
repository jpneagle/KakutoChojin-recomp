// gpu.h on Direct3D 11 (Windows).
#include "gpu_backend.h"

#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <tuple>
#include <unordered_map>

#include "../host.h"
#include "../platform/platform.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace gpu {

namespace d3d11 {

namespace {

struct DxTexture : Texture {
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
};

struct DxShader : Shader {
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11InputLayout* layout = nullptr;
};

DxTexture* Dx(Texture* t) { return static_cast<DxTexture*>(t); }
DxShader* Dx(Shader* s) { return static_cast<DxShader*>(s); }

ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
ID3D11DeviceContext1* g_ctx1 = nullptr;
IDXGISwapChain1* g_swap = nullptr;

// Current state, re-bound after internal draws (clears, blits).
DxTexture* g_rt = nullptr;
DxTexture* g_ds = nullptr;
DxTexture* g_tex[4] = {};
D3D11_VIEWPORT g_vp{};
DxShader* g_vs = nullptr;
DxShader* g_ps = nullptr;
BlendState g_blend;
DepthStencilState g_depth;
RasterState g_raster;
ID3D11Buffer* g_vs_cb[4] = {};

const char* const kHlslPrelude = R"(
#define UNIFORMS_VS(name, slot) cbuffer name : register(b##slot)
#define UNIFORMS_PS(name, slot) cbuffer name : register(b##slot)
#define ROW_MAJOR row_major
#define TEXTURE2D(name, slot) Texture2D name : register(t##slot); SamplerState name##_s : register(s##slot);
#define TEXTURECUBE(name, slot) TextureCube name : register(t##slot); SamplerState name##_s : register(s##slot);
#define SAMPLE(name, coord) name.Sample(name##_s, coord)
#define LOAD2D(name, xy) name.Load(int3(xy, 0))
#define TO3X3(m) ((float3x3)(m))
#define XXXX(s) ((s).xxxx)
#define XXX(s) ((s).xxx)
#define UNROLL [unroll]
float4 SLT(float4 a, float4 b) { return float4(a < b); }
float4 SGE(float4 a, float4 b) { return float4(a >= b); }
struct VSOut {
    float4 pos : SV_Position;
    float4 d0 : COLOR0;
    float4 d1 : COLOR1;
    float4 t0 : TEXCOORD0;
    float4 t1 : TEXCOORD1;
    float4 t2 : TEXCOORD2;
    float4 t3 : TEXCOORD3;
    float fog : TEXCOORD4;
};
)";

DXGI_FORMAT VertexDxgi(VertexFormat f) {
    switch (f) {
        case VertexFormat::Float1: return DXGI_FORMAT_R32_FLOAT;
        case VertexFormat::Float2: return DXGI_FORMAT_R32G32_FLOAT;
        case VertexFormat::Float3: return DXGI_FORMAT_R32G32B32_FLOAT;
        case VertexFormat::Float4: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case VertexFormat::Color: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case VertexFormat::UByte4N: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case VertexFormat::Short2N: return DXGI_FORMAT_R16G16_SNORM;
        case VertexFormat::Short4N: return DXGI_FORMAT_R16G16B16A16_SNORM;
        case VertexFormat::Short2: return DXGI_FORMAT_R16G16_SINT;
        case VertexFormat::Short4: return DXGI_FORMAT_R16G16B16A16_SINT;
    }
    return DXGI_FORMAT_UNKNOWN;
}

DXGI_FORMAT TextureDxgi(Format f) {
    switch (f) {
        case Format::BGRA8: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case Format::BC1: return DXGI_FORMAT_BC1_UNORM;
        case Format::BC2: return DXGI_FORMAT_BC2_UNORM;
        case Format::BC3: return DXGI_FORMAT_BC3_UNORM;
        case Format::D24S8: return DXGI_FORMAT_R24G8_TYPELESS;
    }
    return DXGI_FORMAT_UNKNOWN;
}

ID3DBlob* CompileHlsl(const std::string& src, const char* profile, const char* tag) {
    static std::unordered_map<std::string, ID3DBlob*> cache;
    std::string key = std::string(profile) + src;
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    DumpShader(src, tag, "hlsl");
    ID3DBlob *code = nullptr, *err = nullptr;
    HRESULT hr = D3DCompile(src.data(), src.size(), tag, nullptr, nullptr, "main", profile,
                            D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &err);
    if (FAILED(hr)) {
        Log("%s: HLSL compile failed: %s\n---\n%s\n---", tag, err ? static_cast<const char*>(err->GetBufferPointer()) : "?",
            src.c_str());
        code = nullptr;
    }
    if (err) err->Release();
    cache[key] = code;
    return code;
}

// Entry point around XboxVS: inputs by register, (0, 0, 0, 1) elsewhere.
std::string VertexMain(const std::vector<VertexElement>& inputs) {
    std::string s = "struct VSIn {\n";
    char buf[128];
    for (const VertexElement& e : inputs) {
        snprintf(buf, sizeof buf, "    %s4 r%u : TEXCOORD%u;\n", IsInteger(e.format) ? "int" : "float", e.reg, e.reg);
        s += buf;
    }
    s += "    uint vid : SV_VertexID;\n};\nVSOut main(VSIn i) {\n    float4 v[16];\n";
    s += "    [unroll] for (int k = 0; k < 16; k++) v[k] = float4(0, 0, 0, 1);\n";
    for (const VertexElement& e : inputs) {
        snprintf(buf, sizeof buf, "    v[%u] = float4(i.r%u);\n", e.reg, e.reg);
        s += buf;
    }
    return s + "    VSOut o;\n    XboxVS(v, int(i.vid), o);\n    return o;\n}\n";
}

ID3D11Buffer* ConstantBuffer(Stage stage, uint32_t slot, uint32_t bytes) {
    static std::map<std::tuple<int, uint32_t, uint32_t>, ID3D11Buffer*> buffers;
    ID3D11Buffer*& b = buffers[{int(stage), slot, bytes}];
    if (!b) {
        D3D11_BUFFER_DESC d{(bytes + 15) & ~15u, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
        g_dev->CreateBuffer(&d, nullptr, &b);
    }
    return b;
}

// ---- Dynamic vertex/index data -------------------------------------------------------------

constexpr UINT kVbBytes = 16 << 20, kIbBytes = 4 << 20;

struct DynamicBuffer {
    ID3D11Buffer* buf = nullptr;
    UINT pos = 0;
};
DynamicBuffer g_dyn_vb[16], g_dyn_ib;

UINT Upload(DynamicBuffer& d, UINT capacity, UINT bind, const void* src, UINT bytes, UINT align) {
    if (!d.buf) {
        D3D11_BUFFER_DESC desc{capacity, D3D11_USAGE_DYNAMIC, bind, D3D11_CPU_ACCESS_WRITE};
        g_dev->CreateBuffer(&desc, nullptr, &d.buf);
    }
    if (bytes > capacity) return UINT(-1);
    UINT start = (d.pos + align - 1) / align * align;
    D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
    if (start + bytes > capacity) start = 0, mode = D3D11_MAP_WRITE_DISCARD;
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(g_ctx->Map(d.buf, 0, mode, 0, &m))) return UINT(-1);
    memcpy(static_cast<uint8_t*>(m.pData) + start, src, bytes);
    g_ctx->Unmap(d.buf, 0);
    d.pos = start + bytes;
    return start;
}

D3D11_PRIMITIVE_TOPOLOGY Topo(Topology t) {
    switch (t) {
        case Topology::PointList: return D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
        case Topology::LineList: return D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
        case Topology::LineStrip: return D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
        case Topology::TriangleStrip: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
        default: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    }
}

// ---- State application ------------------------------------------------------------------------

void ApplyBlend(const BlendState& s) {
    D3D11_BLEND_DESC bd{};
    auto& rt = bd.RenderTarget[0];
    rt.BlendEnable = s.enable;
    rt.SrcBlend = D3D11_BLEND(s.src), rt.DestBlend = D3D11_BLEND(s.dst);
    rt.SrcBlendAlpha = D3D11_BLEND(s.src_alpha), rt.DestBlendAlpha = D3D11_BLEND(s.dst_alpha);
    rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP(s.op);
    rt.RenderTargetWriteMask = s.write_mask;
    ID3D11BlendState* bs = nullptr;
    g_dev->CreateBlendState(&bd, &bs);  // D3D11 returns the same object for equal descriptions
    g_ctx->OMSetBlendState(bs, s.factor, 0xFFFFFFFF);
    if (bs) bs->Release();
}

void ApplyDepthStencil(const DepthStencilState& s) {
    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = s.depth_enable;
    dd.DepthWriteMask = s.depth_write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = D3D11_COMPARISON_FUNC(s.depth_func);
    dd.StencilEnable = s.stencil_enable;
    dd.StencilReadMask = s.stencil_read_mask, dd.StencilWriteMask = s.stencil_write_mask;
    dd.FrontFace = {D3D11_STENCIL_OP(s.fail), D3D11_STENCIL_OP(s.depth_fail), D3D11_STENCIL_OP(s.pass),
                    D3D11_COMPARISON_FUNC(s.stencil_func)};
    dd.BackFace = dd.FrontFace;
    ID3D11DepthStencilState* dss = nullptr;
    g_dev->CreateDepthStencilState(&dd, &dss);
    g_ctx->OMSetDepthStencilState(dss, s.stencil_ref);
    if (dss) dss->Release();
}

void ApplyRaster(const RasterState& s) {
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = s.wireframe ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
    rd.CullMode = s.cull_back ? D3D11_CULL_BACK : D3D11_CULL_NONE;
    rd.FrontCounterClockwise = s.front_ccw;
    rd.DepthBias = s.depth_bias;
    rd.SlopeScaledDepthBias = s.slope_bias;
    rd.DepthClipEnable = TRUE;
    ID3D11RasterizerState* rs = nullptr;
    g_dev->CreateRasterizerState(&rd, &rs);
    g_ctx->RSSetState(rs);
    if (rs) rs->Release();
}

void BindTargets() {
    ID3D11ShaderResourceView* none[4] = {};
    g_ctx->PSSetShaderResources(0, 4, none);  // a target may still be bound as an input
    ID3D11RenderTargetView* rtv = g_rt ? g_rt->rtv : nullptr;
    g_ctx->OMSetRenderTargets(1, &rtv, g_ds ? g_ds->dsv : nullptr);
}

// Re-binds the draw state after an internal draw.
void Restore() {
    BindTargets();
    g_ctx->RSSetViewports(1, &g_vp);
    ApplyBlend(g_blend);
    ApplyDepthStencil(g_depth);
    ApplyRaster(g_raster);
    if (g_vs) g_ctx->VSSetShader(g_vs->vs, nullptr, 0), g_ctx->IASetInputLayout(g_vs->layout);
    if (g_ps) g_ctx->PSSetShader(g_ps->ps, nullptr, 0);
    for (UINT i = 0; i < 4; i++)
        if (g_vs_cb[i]) g_ctx->VSSetConstantBuffers(i, 1, &g_vs_cb[i]);
}

// Internal full-screen-quad shaders (blits and depth clears).
struct Internal {
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
};

Internal MakeInternal(const char* src, const char* vs_main, const char* ps_main, const char* tag) {
    Internal r;
    std::string base = std::string(src) + "\n";
    ID3DBlob* v = CompileHlsl(base + vs_main, "vs_4_0", tag);
    ID3DBlob* p = CompileHlsl(base + ps_main, "ps_4_0", tag);
    if (v) g_dev->CreateVertexShader(v->GetBufferPointer(), v->GetBufferSize(), nullptr, &r.vs);
    if (p) g_dev->CreatePixelShader(p->GetBufferPointer(), p->GetBufferSize(), nullptr, &r.ps);
    return r;
}

// Draws a texture stretched over a viewport of `dst` with bilinear filtering.
void Blit(ID3D11ShaderResourceView* src, ID3D11RenderTargetView* dst, const D3D11_VIEWPORT& vp) {
    static Internal sh;
    static ID3D11SamplerState* samp = nullptr;
    if (!sh.vs) {
        sh = MakeInternal(R"(
Texture2D src : register(t0);
SamplerState samp : register(s0);
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vsmain(uint id) {
    V o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
)",
                          "V main(uint id : SV_VertexID) { return vsmain(id); }\n",
                          "float4 main(V i) : SV_Target { return src.Sample(samp, i.uv); }\n", "blit");
        D3D11_SAMPLER_DESC sd{D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP,
                              D3D11_TEXTURE_ADDRESS_CLAMP};
        g_dev->CreateSamplerState(&sd, &samp);
    }
    if (!sh.vs || !sh.ps) return;
    ID3D11ShaderResourceView* none = nullptr;
    g_ctx->OMSetRenderTargets(1, &dst, nullptr);
    g_ctx->RSSetViewports(1, &vp);
    g_ctx->RSSetState(nullptr);
    g_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    g_ctx->OMSetDepthStencilState(nullptr, 0);
    g_ctx->IASetInputLayout(nullptr);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(sh.vs, nullptr, 0);
    g_ctx->PSSetShader(sh.ps, nullptr, 0);
    g_ctx->PSSetShaderResources(0, 1, &src);
    g_ctx->PSSetSamplers(0, 1, &samp);
    g_ctx->Draw(3, 0);
    g_ctx->PSSetShaderResources(0, 1, &none);
}

// D3D11 can only clear whole depth views, so rectangles are cleared by
// drawing a quad that writes depth/stencil only.
void ClearDepthRect(const D3D11_RECT& r, bool depth, bool stencil, float z, UINT8 s) {
    static Internal sh;
    static ID3D11Buffer* cb = nullptr;
    if (!sh.vs) {
        sh = MakeInternal(R"(
cbuffer C : register(b0) { float4 depth; };
float4 vsmain(uint id) { return float4((id & 1) ? 1 : -1, (id & 2) ? -1 : 1, depth.x, 1); }
)",
                          "float4 main(uint id : SV_VertexID) : SV_Position { return vsmain(id); }\n",
                          "void main() {}\n", "depthclear");
        D3D11_BUFFER_DESC bd{16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
        g_dev->CreateBuffer(&bd, nullptr, &cb);
    }
    if (!sh.vs || !sh.ps) return;
    float c[4] = {z, 0, 0, 0};
    g_ctx->UpdateSubresource(cb, 0, nullptr, c, 0, 0);
    DepthStencilState ds;
    ds.depth_enable = depth, ds.depth_write = true, ds.depth_func = Compare::Always;
    ds.stencil_enable = stencil, ds.stencil_ref = s;
    ds.fail = ds.depth_fail = ds.pass = StencilOp::Replace;
    ApplyDepthStencil(ds);
    BlendState bs;
    bs.write_mask = 0;
    ApplyBlend(bs);
    D3D11_VIEWPORT vp{float(r.left), float(r.top), float(r.right - r.left), float(r.bottom - r.top), 0, 1};
    g_ctx->RSSetViewports(1, &vp);
    g_ctx->RSSetState(nullptr);
    g_ctx->IASetInputLayout(nullptr);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    g_ctx->VSSetShader(sh.vs, nullptr, 0);
    g_ctx->VSSetConstantBuffers(0, 1, &cb);
    g_ctx->PSSetShader(sh.ps, nullptr, 0);
    g_ctx->Draw(4, 0);
}

}  // namespace

// ---- Backend entry points -------------------------------------------------------------------------

bool CreateDevice() {
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   nullptr, 0, D3D11_SDK_VERSION, &g_dev, &fl, &g_ctx);
    if (FAILED(hr)) {
        Log("hardware D3D11 device unavailable (%08lx); using WARP", hr);
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                               D3D11_SDK_VERSION, &g_dev, &fl, &g_ctx);
    }
    if (FAILED(hr)) return false;
    Log("D3D11 device created (feature level %x)", fl);
    g_ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&g_ctx1));

    HWND hwnd = static_cast<HWND>(platform::NativeWindowHandle());
    if (!hwnd) return true;
    IDXGIDevice* xdev = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory2* factory = nullptr;
    g_dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&xdev));
    xdev->GetAdapter(&adapter);
    adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory));
    int cw, ch;
    platform::DrawableSize(&cw, &ch);
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = std::max(cw, 1), sd.Height = std::max(ch, 1), sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT, sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    hr = factory->CreateSwapChainForHwnd(g_dev, hwnd, &sd, nullptr, nullptr, &g_swap);
    if (FAILED(hr)) Log("no swap chain (%08lx); rendering offscreen only", hr);
    if (g_swap) factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);  // fullscreen is borderless (platform)
    factory->Release(), adapter->Release(), xdev->Release();
    return true;
}

Texture* CreateTexture(uint32_t w, uint32_t h, uint32_t levels, Format f, bool cube, const SubresourceData* init) {
    UINT faces = cube ? 6 : 1;
    std::vector<D3D11_SUBRESOURCE_DATA> data(faces * levels);
    for (UINT i = 0; i < faces * levels; i++) data[i] = {init[i].data, init[i].row_pitch, 0};
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w, d.Height = h, d.MipLevels = levels, d.ArraySize = faces, d.SampleDesc.Count = 1;
    d.Format = TextureDxgi(f), d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    d.MiscFlags = cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
    auto* t = new DxTexture;
    HRESULT hr = g_dev->CreateTexture2D(&d, data.data(), &t->tex);
    if (FAILED(hr)) {
        Log("CreateTexture2D(%ux%u x%u fmt %d) failed: %08lx", w, h, levels, int(f), hr);
        delete t;
        return nullptr;
    }
    g_dev->CreateShaderResourceView(t->tex, nullptr, &t->srv);
    t->w = w, t->h = h, t->format = f;
    return t;
}

Texture* CreateRenderTarget(uint32_t w, uint32_t h, bool depth) {
    auto* t = new DxTexture;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w, d.Height = h, d.MipLevels = 1, d.ArraySize = 1, d.SampleDesc.Count = 1;
    d.Format = depth ? DXGI_FORMAT_R24G8_TYPELESS : DXGI_FORMAT_B8G8R8A8_UNORM;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | (depth ? D3D11_BIND_DEPTH_STENCIL : D3D11_BIND_RENDER_TARGET);
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &t->tex))) Fatal("CreateTexture2D(target %ux%u) failed", w, h);
    if (depth) {
        D3D11_DEPTH_STENCIL_VIEW_DESC dv{DXGI_FORMAT_D24_UNORM_S8_UINT, D3D11_DSV_DIMENSION_TEXTURE2D};
        g_dev->CreateDepthStencilView(t->tex, &dv, &t->dsv);
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{DXGI_FORMAT_R24_UNORM_X8_TYPELESS, D3D11_SRV_DIMENSION_TEXTURE2D};
        sv.Texture2D.MipLevels = 1;
        g_dev->CreateShaderResourceView(t->tex, &sv, &t->srv);
    } else {
        g_dev->CreateRenderTargetView(t->tex, nullptr, &t->rtv);
        g_dev->CreateShaderResourceView(t->tex, nullptr, &t->srv);
    }
    t->w = w, t->h = h, t->format = depth ? Format::D24S8 : Format::BGRA8;
    return t;
}

void DestroyTexture(Texture* base) {
    if (!base) return;
    DxTexture* t = Dx(base);
    for (auto*& b : g_tex)
        if (b == t) b = nullptr;
    if (t->srv) t->srv->Release();
    if (t->rtv) t->rtv->Release();
    if (t->dsv) t->dsv->Release();
    if (t->tex) t->tex->Release();
    delete t;
}

void UpdateTexture(Texture* t, const Rect& r, const void* bgra, uint32_t pitch) {
    D3D11_BOX box{UINT(r.left), UINT(r.top), 0, UINT(r.right), UINT(r.bottom), 1};
    g_ctx->UpdateSubresource(Dx(t)->tex, 0, &box, bgra, pitch, 0);
}

void CopyTexture(Texture* dst, int dx, int dy, Texture* src, const Rect& r) {
    if (dx == 0 && dy == 0 && r.left == 0 && r.top == 0 && UINT(r.right) == src->w && UINT(r.bottom) == src->h &&
        src->w == dst->w && src->h == dst->h) {
        g_ctx->CopyResource(Dx(dst)->tex, Dx(src)->tex);
        return;
    }
    D3D11_BOX box{UINT(r.left), UINT(r.top), 0, UINT(r.right), UINT(r.bottom), 1};
    g_ctx->CopySubresourceRegion(Dx(dst)->tex, 0, dx, dy, 0, Dx(src)->tex, 0, &box);
}

bool ReadTexture(Texture* src, const Rect& r, std::vector<uint8_t>* bgra) {
    UINT w = r.right - r.left, h = r.bottom - r.top;
    D3D11_TEXTURE2D_DESC d{w, h, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ};
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &staging))) return false;
    D3D11_BOX box{UINT(r.left), UINT(r.top), 0, UINT(r.right), UINT(r.bottom), 1};
    g_ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, Dx(src)->tex, 0, &box);
    D3D11_MAPPED_SUBRESOURCE m;
    bool ok = SUCCEEDED(g_ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m));
    if (ok) {
        bgra->resize(size_t(w) * h * 4);
        for (UINT y = 0; y < h; y++) memcpy(bgra->data() + size_t(y) * w * 4, static_cast<uint8_t*>(m.pData) + y * m.RowPitch, w * 4);
        g_ctx->Unmap(staging, 0);
    }
    staging->Release();
    return ok;
}

Shader* CompileVertexShader(const std::string& src, const std::vector<VertexElement>& inputs, const char* tag) {
    static std::map<std::pair<std::string, std::string>, Shader*> cache;
    std::string sig;
    for (const VertexElement& e : inputs) {
        char buf[48];
        snprintf(buf, sizeof buf, "%u:%u:%u:%d;", e.reg, e.stream, e.offset, int(e.format));
        sig += buf;
    }
    auto key = std::make_pair(src, sig);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    Shader* s = nullptr;
    if (ID3DBlob* blob = CompileHlsl(kHlslPrelude + src + VertexMain(inputs), "vs_4_0", tag)) {
        static std::unordered_map<ID3DBlob*, ID3D11VertexShader*> objects;
        ID3D11VertexShader*& vs = objects[blob];
        if (!vs) g_dev->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &vs);
        std::vector<D3D11_INPUT_ELEMENT_DESC> desc;
        for (const VertexElement& e : inputs)
            desc.push_back({"TEXCOORD", e.reg, VertexDxgi(e.format), e.stream, e.offset, D3D11_INPUT_PER_VERTEX_DATA, 0});
        ID3D11InputLayout* il = nullptr;
        if (!desc.empty() && FAILED(g_dev->CreateInputLayout(desc.data(), UINT(desc.size()), blob->GetBufferPointer(),
                                                             blob->GetBufferSize(), &il)))
            Log("%s: CreateInputLayout failed for %s", tag, sig.c_str());
        auto* d = new DxShader;
        d->vs = vs, d->layout = il;
        s = d;
    }
    cache[key] = s;
    return s;
}

Shader* CompilePixelShader(const std::string& src, const char* tag) {
    static std::unordered_map<std::string, Shader*> cache;
    auto it = cache.find(src);
    if (it != cache.end()) return it->second;
    Shader* s = nullptr;
    if (ID3DBlob* blob = CompileHlsl(kHlslPrelude + src + "float4 main(VSOut i) : SV_Target { return XboxPS(i); }\n",
                                     "ps_4_0", tag)) {
        auto* d = new DxShader;
        g_dev->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &d->ps);
        s = d;
    }
    cache[src] = s;
    return s;
}

void SetRenderTargets(Texture* color, Texture* depth) {
    g_rt = Dx(color), g_ds = Dx(depth);
    BindTargets();
}

void SetViewport(const Viewport& v) {
    g_vp = {v.x, v.y, v.w, v.h, v.min_z, v.max_z};
    g_ctx->RSSetViewports(1, &g_vp);
}

void SetBlendState(const BlendState& s) { g_blend = s, ApplyBlend(s); }
void SetDepthStencilState(const DepthStencilState& s) { g_depth = s, ApplyDepthStencil(s); }
void SetRasterState(const RasterState& s) { g_raster = s, ApplyRaster(s); }

void SetTexture(uint32_t slot, Texture* t) {
    if (slot >= 4) return;
    g_tex[slot] = Dx(t);
    ID3D11ShaderResourceView* srv = t && t != g_rt && t != g_ds ? Dx(t)->srv : nullptr;
    g_ctx->PSSetShaderResources(slot, 1, &srv);
}

void SetSampler(uint32_t slot, const SamplerState& s) {
    D3D11_SAMPLER_DESC sd{};
    if (s.anisotropic)
        sd.Filter = D3D11_FILTER_ANISOTROPIC;
    else
        sd.Filter = D3D11_FILTER((s.min_linear ? 0x10 : 0) | (s.mag_linear ? 0x4 : 0) | (s.mip_linear ? 0x1 : 0));
    sd.AddressU = D3D11_TEXTURE_ADDRESS_MODE(s.u);
    sd.AddressV = D3D11_TEXTURE_ADDRESS_MODE(s.v);
    sd.AddressW = D3D11_TEXTURE_ADDRESS_MODE(s.w);
    sd.MipLODBias = s.lod_bias;
    sd.MaxAnisotropy = s.max_anisotropy;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    memcpy(sd.BorderColor, s.border, 16);
    sd.MinLOD = s.min_lod, sd.MaxLOD = s.max_lod;
    ID3D11SamplerState* ss = nullptr;
    g_dev->CreateSamplerState(&sd, &ss);
    g_ctx->PSSetSamplers(slot, 1, &ss);
    if (ss) ss->Release();
}

void SetConstants(Stage stage, uint32_t slot, const void* data, uint32_t bytes) {
    ID3D11Buffer* b = ConstantBuffer(stage, slot, bytes);
    if ((bytes & 15) == 0) {
        g_ctx->UpdateSubresource(b, 0, nullptr, data, 0, 0);
    } else {
        std::vector<uint8_t> padded((bytes + 15) & ~15u);
        memcpy(padded.data(), data, bytes);
        g_ctx->UpdateSubresource(b, 0, nullptr, padded.data(), 0, 0);
    }
    if (stage == Stage::Vertex) {
        g_ctx->VSSetConstantBuffers(slot, 1, &b);
        if (slot < 4) g_vs_cb[slot] = b;
    } else {
        g_ctx->PSSetConstantBuffers(slot, 1, &b);
    }
}

void SetShaders(Shader* vs, Shader* ps) {
    g_vs = Dx(vs), g_ps = Dx(ps);
    g_ctx->VSSetShader(g_vs ? g_vs->vs : nullptr, nullptr, 0);
    g_ctx->IASetInputLayout(g_vs ? g_vs->layout : nullptr);
    g_ctx->PSSetShader(g_ps ? g_ps->ps : nullptr, nullptr, 0);
}

void SetVertexStream(uint32_t stream, const void* data, uint32_t bytes, uint32_t stride) {
    if (stream >= 16) return;
    ID3D11Buffer* none = nullptr;
    UINT zero = 0;
    if (!data || !bytes) {
        g_ctx->IASetVertexBuffers(stream, 1, &none, &zero, &zero);
        return;
    }
    UINT off = Upload(g_dyn_vb[stream], kVbBytes, D3D11_BIND_VERTEX_BUFFER, data, bytes, 16);
    if (off == UINT(-1)) {
        g_ctx->IASetVertexBuffers(stream, 1, &none, &zero, &zero);
        return;
    }
    g_ctx->IASetVertexBuffers(stream, 1, &g_dyn_vb[stream].buf, &stride, &off);
}

void Draw(Topology t, uint32_t vertices) {
    g_ctx->IASetPrimitiveTopology(Topo(t));
    g_ctx->Draw(vertices, 0);
}

void DrawIndexed(Topology t, const uint16_t* indices, uint32_t count) {
    UINT off = Upload(g_dyn_ib, kIbBytes, D3D11_BIND_INDEX_BUFFER, indices, count * 2, 2);
    if (off == UINT(-1)) return;
    g_ctx->IASetPrimitiveTopology(Topo(t));
    g_ctx->IASetIndexBuffer(g_dyn_ib.buf, DXGI_FORMAT_R16_UINT, 0);
    g_ctx->DrawIndexed(count, off / 2, 0);
}

void Clear(const Rect* rects, uint32_t count, const float* color, const float* depth, const uint8_t* stencil) {
    Texture* any = g_rt ? g_rt : g_ds;
    if (!any) return;
    std::vector<D3D11_RECT> rs;
    for (uint32_t i = 0; rects && i < count; i++) rs.push_back({rects[i].left, rects[i].top, rects[i].right, rects[i].bottom});
    bool full = !rects || (rs.size() == 1 && rs[0].left <= 0 && rs[0].top <= 0 && rs[0].right >= LONG(any->w) &&
                           rs[0].bottom >= LONG(any->h));
    if (color && g_rt && g_rt->rtv) {
        if (full || !g_ctx1)
            g_ctx->ClearRenderTargetView(g_rt->rtv, color);
        else
            g_ctx1->ClearView(g_rt->rtv, color, rs.data(), UINT(rs.size()));
    }
    if ((depth || stencil) && g_ds && g_ds->dsv) {
        float z = depth ? *depth : 0;
        UINT8 s = stencil ? *stencil : 0;
        if (full) {
            UINT f = (depth ? D3D11_CLEAR_DEPTH : 0) | (stencil ? D3D11_CLEAR_STENCIL : 0);
            g_ctx->ClearDepthStencilView(g_ds->dsv, f, z, s);
        } else {
            for (const D3D11_RECT& r : rs) ClearDepthRect(r, depth != nullptr, stencil != nullptr, z, s);
            Restore();
        }
    }
}

void BlitTexture(Texture* src, Texture* dst) {
    if (src->w == dst->w && src->h == dst->h) {
        g_ctx->CopyResource(Dx(dst)->tex, Dx(src)->tex);
        return;
    }
    Blit(Dx(src)->srv, Dx(dst)->rtv, {0, 0, float(dst->w), float(dst->h), 0, 1});
    Restore();
}

void Present(Texture* image) {
    if (!g_swap) return;
    int cw, ch;
    if (platform::TakeResize(&cw, &ch) && cw > 0 && ch > 0) {
        g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
        g_ctx->Flush();
        HRESULT hr = g_swap->ResizeBuffers(0, cw, ch, DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(hr)) Log("ResizeBuffers(%dx%d) failed: %08lx", cw, ch, hr);
    }
    ID3D11Texture2D* bb = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb));
    if (bb && SUCCEEDED(g_dev->CreateRenderTargetView(bb, nullptr, &rtv))) {
        D3D11_TEXTURE2D_DESC d;
        bb->GetDesc(&d);
        if (d.Width == image->w && d.Height == image->h) {
            g_ctx->CopyResource(bb, Dx(image)->tex);
        } else {
            float black[4] = {0, 0, 0, 1};
            g_ctx->ClearRenderTargetView(rtv, black);
            float s = std::min(float(d.Width) / image->w, float(d.Height) / image->h);
            float w = std::floor(image->w * s), h = std::floor(image->h * s);
            Blit(Dx(image)->srv, rtv, {std::floor((d.Width - w) / 2), std::floor((d.Height - h) / 2), w, h, 0, 1});
        }
        rtv->Release();
    }
    if (bb) bb->Release();
    // Frame timing is the caller's (vsync would add the monitor's rate on top).
    g_swap->Present(0, 0);
    Restore();
}

}  // namespace d3d11

}  // namespace gpu
