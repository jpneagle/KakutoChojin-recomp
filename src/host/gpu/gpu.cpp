// gpu.h front end: backend selection and dispatch.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include "../host.h"
#include "gpu_backend.h"

namespace gpu {

namespace {

enum class Backend { D3D11, GL };
Backend g_backend = Backend::GL;

// KT_GPU in the environment, else [video] gpu in KakutoChojin.ini.
std::string BackendOption() {
    if (const char* v = os::Env("KT_GPU")) return v;
    return os::IniString("video", "gpu", "");
}

}  // namespace

#if defined(_WIN32) && defined(KT_GPU_GL)
#define KT_DISPATCH(call) (g_backend == Backend::D3D11 ? d3d11::call : gl::call)
#elif defined(_WIN32)
#define KT_DISPATCH(call) (d3d11::call)
#else
#define KT_DISPATCH(call) (gl::call)
#endif

void CreateDevice() {
    std::string want = BackendOption();
#if defined(_WIN32) && defined(KT_GPU_GL)
    g_backend = want == "gl" || want == "opengl" ? Backend::GL : Backend::D3D11;
#elif defined(_WIN32)
    g_backend = Backend::D3D11;
#else
    g_backend = Backend::GL;
#endif
    if (!KT_DISPATCH(CreateDevice())) Fatal("%s device creation failed", BackendName());
    Log("gpu: %s backend", BackendName());
}

const char* BackendName() { return g_backend == Backend::D3D11 ? "Direct3D 11" : "OpenGL"; }

void DumpShader(const std::string& src, const char* tag, const char* ext) {
    static const bool dump = os::EnvSet("KT_DUMP_SHADERS");
    if (!dump) return;
    static int n = 0;
    std::error_code ec;
    std::filesystem::create_directories(HostDir() / "shaders", ec);
    char name[128];
    snprintf(name, sizeof name, "%04d_%s.%s", n++, tag, ext);
    if (FILE* f = os::OpenFile(HostDir() / "shaders" / name, "w")) fwrite(src.data(), 1, src.size(), f), fclose(f);
}

Texture* CreateTexture(uint32_t w, uint32_t h, uint32_t levels, Format f, bool cube, const SubresourceData* init) {
    return KT_DISPATCH(CreateTexture(w, h, levels, f, cube, init));
}
Texture* CreateRenderTarget(uint32_t w, uint32_t h, bool depth) { return KT_DISPATCH(CreateRenderTarget(w, h, depth)); }
void DestroyTexture(Texture* t) { KT_DISPATCH(DestroyTexture(t)); }
void UpdateTexture(Texture* t, const Rect& r, const void* bgra, uint32_t pitch) {
    KT_DISPATCH(UpdateTexture(t, r, bgra, pitch));
}
void CopyTexture(Texture* dst, int dx, int dy, Texture* src, const Rect& r) { KT_DISPATCH(CopyTexture(dst, dx, dy, src, r)); }
bool ReadTexture(Texture* src, const Rect& r, std::vector<uint8_t>* bgra) { return KT_DISPATCH(ReadTexture(src, r, bgra)); }
Shader* CompileVertexShader(const std::string& src, const std::vector<VertexElement>& inputs, const char* tag) {
    return KT_DISPATCH(CompileVertexShader(src, inputs, tag));
}
Shader* CompilePixelShader(const std::string& src, const char* tag) { return KT_DISPATCH(CompilePixelShader(src, tag)); }
void SetRenderTargets(Texture* color, Texture* depth) { KT_DISPATCH(SetRenderTargets(color, depth)); }
void SetViewport(const Viewport& vp) { KT_DISPATCH(SetViewport(vp)); }
void SetBlendState(const BlendState& s) { KT_DISPATCH(SetBlendState(s)); }
void SetDepthStencilState(const DepthStencilState& s) { KT_DISPATCH(SetDepthStencilState(s)); }
void SetRasterState(const RasterState& s) { KT_DISPATCH(SetRasterState(s)); }
void SetTexture(uint32_t slot, Texture* t) { KT_DISPATCH(SetTexture(slot, t)); }
void SetSampler(uint32_t slot, const SamplerState& s) { KT_DISPATCH(SetSampler(slot, s)); }
void SetConstants(Stage stage, uint32_t slot, const void* data, uint32_t bytes) {
    KT_DISPATCH(SetConstants(stage, slot, data, bytes));
}
void SetShaders(Shader* vs, Shader* ps) { KT_DISPATCH(SetShaders(vs, ps)); }
void SetVertexStream(uint32_t stream, const void* data, uint32_t bytes, uint32_t stride) {
    KT_DISPATCH(SetVertexStream(stream, data, bytes, stride));
}
void Draw(Topology t, uint32_t vertices) { KT_DISPATCH(Draw(t, vertices)); }
void DrawIndexed(Topology t, const uint16_t* indices, uint32_t count) { KT_DISPATCH(DrawIndexed(t, indices, count)); }
void Clear(const Rect* rects, uint32_t count, const float* color, const float* depth, const uint8_t* stencil) {
    KT_DISPATCH(Clear(rects, count, color, depth, stencil));
}
void BlitTexture(Texture* src, Texture* dst) { KT_DISPATCH(BlitTexture(src, dst)); }
void Present(Texture* image) { KT_DISPATCH(Present(image)); }

}  // namespace gpu
