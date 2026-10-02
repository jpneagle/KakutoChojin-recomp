// gpu.h on OpenGL 4.5 core (SDL3 context).
//
// The title calls D3D from several threads, but a GL context is current on
// one thread only, so every GL call runs on a render thread. Callers record
// commands into batches: state setters only update a shadow copy, and each
// draw carries a snapshot of it; vertex data, indices and constants go into
// the batch's arena, which is uploaded once per batch. The render thread
// applies only state that changed. Calls that return results (shader
// compilation, read-back) wait for the render thread.
//
// Conventions match D3D: glClipControl(UPPER_LEFT, ZERO_TO_ONE) makes row 0
// of every framebuffer and texture the top row, so only presenting to the
// window (whose row 0 is the bottom) flips.
#include "gpu_backend.h"

#define GL_GLEXT_PROTOTYPES
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "../host.h"
#include "../platform/platform.h"

#ifndef GL_TEXTURE_MAX_ANISOTROPY
#define GL_TEXTURE_MAX_ANISOTROPY 0x84FE
#endif

namespace gpu {
namespace gl {

namespace {

// ---- GL entry points (loaded through SDL) ------------------------------------------------------

#define KT_GL_FUNCTIONS(X)                                                                                         \
    X(glGetError) X(glGetIntegerv) X(glGetString) X(glEnable) X(glDisable) X(glClearColor) X(glClear)             \
    X(glColorMask) X(glDepthMask) X(glDepthFunc) X(glStencilMask) X(glStencilFunc) X(glStencilOp) X(glCullFace)   \
    X(glFrontFace) X(glPolygonMode) X(glPolygonOffset) X(glScissor) X(glPixelStorei) X(glDrawArrays)             \
    X(glDrawElements) X(glDeleteTextures) X(glFlush) X(glFinish) X(glBlendFuncSeparate) X(glBlendEquation)        \
    X(glBlendColor) X(glClipControl) X(glViewportIndexedf) X(glDepthRangeIndexed) X(glCreateTextures)            \
    X(glTextureStorage2D) X(glTextureSubImage2D) X(glTextureSubImage3D) X(glCompressedTextureSubImage2D)         \
    X(glCompressedTextureSubImage3D) X(glTextureParameteri) X(glBindTextureUnit) X(glGetTextureSubImage)         \
    X(glCopyImageSubData) X(glCreateFramebuffers) X(glDeleteFramebuffers) X(glNamedFramebufferTexture)           \
    X(glCheckNamedFramebufferStatus) X(glBindFramebuffer) X(glBlitNamedFramebuffer)                              \
    X(glClearNamedFramebufferfv) X(glClearNamedFramebufferfi) X(glClearNamedFramebufferiv) X(glCreateBuffers)    \
    X(glNamedBufferData) X(glBindBufferRange) X(glCreateVertexArrays) X(glBindVertexArray)                      \
    X(glEnableVertexAttribArray) X(glDisableVertexAttribArray) X(glVertexAttribFormat) X(glVertexAttribIFormat)  \
    X(glVertexAttribBinding) X(glBindVertexBuffer) X(glVertexArrayElementBuffer) X(glCreateShader)              \
    X(glShaderSource) X(glCompileShader) X(glGetShaderiv) X(glGetShaderInfoLog) X(glDeleteShader)                \
    X(glCreateProgram) X(glAttachShader) X(glLinkProgram) X(glGetProgramiv) X(glGetProgramInfoLog)              \
    X(glUseProgram) X(glCreateSamplers) X(glSamplerParameteri) X(glSamplerParameterf) X(glSamplerParameterfv)   \
    X(glBindSampler) X(glDebugMessageCallback)

struct Api {
#define KT_GL_MEMBER(name) decltype(&::name) name = nullptr;
    KT_GL_FUNCTIONS(KT_GL_MEMBER)
#undef KT_GL_MEMBER
};
Api g;

bool LoadApi() {
    bool ok = true;
#define KT_GL_LOAD(name)                                                                     \
    g.name = reinterpret_cast<decltype(g.name)>(SDL_GL_GetProcAddress(#name));               \
    if (!g.name && strcmp(#name, "glDebugMessageCallback") != 0) {                           \
        Log("OpenGL: %s is missing", #name);                                                 \
        ok = false;                                                                          \
    }
    KT_GL_FUNCTIONS(KT_GL_LOAD)
#undef KT_GL_LOAD
    return ok;
}

// ---- Objects --------------------------------------------------------------------------------------

struct GlTexture : Texture {
    GLuint name = 0;
    bool cube = false, target = false;
};

struct GlShader : Shader {
    GLuint name = 0;
    bool vertex = false;
    std::vector<VertexElement> inputs;
};

GlTexture* Gl(Texture* t) { return static_cast<GlTexture*>(t); }
GlShader* Gl(Shader* s) { return static_cast<GlShader*>(s); }

// ---- Recorded state and commands -----------------------------------------------------------------

struct Range {
    uint32_t offset = 0, bytes = 0;
};

struct StreamBinding {
    uint32_t offset = 0, bytes = 0, stride = 0;
};

// Everything a draw depends on (snapshotted into each draw command).
struct DrawState {
    GlTexture* rt = nullptr;
    GlTexture* ds = nullptr;
    Viewport vp{0, 0, 1, 1, 0, 1};
    BlendState blend;
    DepthStencilState depth;
    RasterState raster;
    GlTexture* tex[4] = {};
    SamplerState samp[4];
    GlShader* vs = nullptr;
    GlShader* ps = nullptr;
    Range consts[2][4];
    StreamBinding streams[16];
};

enum class Kind : uint8_t { Draw, Clear, Fn };

struct Cmd {
    Kind kind;
    Topology topo = Topology::TriangleList;
    uint32_t count = 0;
    bool indexed = false;
    uint32_t index_offset = 0;
    // Clear
    uint32_t rect_offset = 0, rect_count = 0;
    bool has_color = false, has_depth = false, has_stencil = false;
    float color[4] = {};
    float z = 0;
    uint8_t stencil = 0;
    DrawState state;
    std::function<void()> fn;
};

struct Batch {
    std::vector<Cmd> cmds;
    std::vector<uint8_t> arena;
};

// Producer side (callers hold the D3D lock).
DrawState g_state;
Batch* g_batch = nullptr;
uint32_t g_ubo_align = 256;
// Last constants copied into the current batch, reused while unchanged.
std::vector<uint8_t> g_last_consts[2][4];
Range g_last_range[2][4];

// Queue between callers and the render thread.
std::mutex g_mu;
std::condition_variable g_cv;
std::deque<Batch*> g_queue;
std::vector<Batch*> g_free;
uint64_t g_submitted = 0, g_completed = 0;
std::atomic<uint64_t> g_presented{0};
uint64_t g_present_requests = 0;

constexpr size_t kBatchCommands = 1024, kBatchArena = 8 << 20;

Batch* NewBatch() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_free.empty()) {
        Batch* b = g_free.back();
        g_free.pop_back();
        return b;
    }
    auto* b = new Batch;
    b->cmds.reserve(kBatchCommands);
    b->arena.reserve(kBatchArena);
    return b;
}

uint32_t ArenaPut(const void* data, uint32_t bytes, uint32_t align, uint32_t alloc = 0);

// Hands the current batch to the render thread. Data that later draws still
// reference (the current constants) is copied into the next batch's arena.
void Submit() {
    if (!g_batch || g_batch->cmds.empty()) return;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_queue.push_back(g_batch);
        g_submitted++;
    }
    g_cv.notify_all();
    g_batch = NewBatch();
    for (int st = 0; st < 2; st++)
        for (int slot = 0; slot < 4; slot++) {
            std::vector<uint8_t>& data = g_last_consts[st][slot];
            if (data.empty()) continue;
            Range r{ArenaPut(data.data(), uint32_t(data.size()), g_ubo_align, (uint32_t(data.size()) + 15) & ~15u),
                    uint32_t(data.size())};
            g_last_range[st][slot] = r;
            if (g_state.consts[st][slot].bytes) g_state.consts[st][slot] = r;
        }
    for (StreamBinding& b : g_state.streams) b = {};  // set again before every draw
}

// Submits once the batch is large (only between commands, so a command and
// the arena data it references always travel together).
void MaybeSubmit() {
    if (g_batch && (g_batch->cmds.size() >= kBatchCommands || g_batch->arena.size() >= kBatchArena)) Submit();
}

// Waits until the render thread has executed everything submitted.
void WaitIdle() {
    Submit();
    std::unique_lock<std::mutex> lk(g_mu);
    uint64_t target = g_submitted;
    g_cv.wait(lk, [&] { return g_completed >= target; });
}

Cmd& Push(Kind k) {
    if (!g_batch) g_batch = NewBatch();
    g_batch->cmds.emplace_back();
    Cmd& c = g_batch->cmds.back();
    c.kind = k;
    return c;
}

void PushFn(std::function<void()> fn) {
    Push(Kind::Fn).fn = std::move(fn);
    MaybeSubmit();
}

// Runs `fn` on the render thread and waits for it.
void RunSync(std::function<void()> fn) {
    PushFn(std::move(fn));
    WaitIdle();
}

// Copies `bytes` into the batch arena (reserving `alloc` bytes if larger).
uint32_t ArenaPut(const void* data, uint32_t bytes, uint32_t align, uint32_t alloc) {
    if (!g_batch) g_batch = NewBatch();
    std::vector<uint8_t>& a = g_batch->arena;
    size_t off = (a.size() + align - 1) / align * align;
    a.resize(off + std::max(bytes, alloc));
    if (data) memcpy(a.data() + off, data, bytes);
    return uint32_t(off);
}

// ---- Render thread -------------------------------------------------------------------------------

SDL_GLContext g_context = nullptr;
GLuint g_vao = 0;
GLuint g_stream_buffers[4] = {};
int g_stream_next = 0;
GLuint g_stream = 0;  // the current batch's arena
GLuint g_dummy2d = 0, g_dummycube = 0;
std::map<std::pair<GLuint, GLuint>, GLuint> g_fbos;
std::map<std::pair<GLuint, GLuint>, GLuint> g_programs;
std::map<std::string, GLuint> g_samplers;

// What the GL context currently has (to skip redundant calls).
struct Applied {
    bool valid = false;
    GLuint fbo = ~0u, program = ~0u;
    GlShader* layout_vs = nullptr;
    Viewport vp{};
    BlendState blend;
    DepthStencilState depth;
    RasterState raster;
    GLuint units[4] = {~0u, ~0u, ~0u, ~0u};
    GLuint samplers[4] = {~0u, ~0u, ~0u, ~0u};
} g_applied;

GLenum Gl(Compare c) {
    static const GLenum k[] = {GL_NEVER, GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS};
    return k[int(c) <= 8 ? int(c) : 8];
}

GLenum Gl(Blend b) {
    switch (b) {
        case Blend::Zero: return GL_ZERO;
        case Blend::One: return GL_ONE;
        case Blend::SrcColor: return GL_SRC_COLOR;
        case Blend::InvSrcColor: return GL_ONE_MINUS_SRC_COLOR;
        case Blend::SrcAlpha: return GL_SRC_ALPHA;
        case Blend::InvSrcAlpha: return GL_ONE_MINUS_SRC_ALPHA;
        case Blend::DestAlpha: return GL_DST_ALPHA;
        case Blend::InvDestAlpha: return GL_ONE_MINUS_DST_ALPHA;
        case Blend::DestColor: return GL_DST_COLOR;
        case Blend::InvDestColor: return GL_ONE_MINUS_DST_COLOR;
        case Blend::SrcAlphaSat: return GL_SRC_ALPHA_SATURATE;
        case Blend::Factor: return GL_CONSTANT_COLOR;
        case Blend::InvFactor: return GL_ONE_MINUS_CONSTANT_COLOR;
    }
    return GL_ONE;
}

GLenum Gl(BlendOp o) {
    switch (o) {
        case BlendOp::Subtract: return GL_FUNC_SUBTRACT;
        case BlendOp::RevSubtract: return GL_FUNC_REVERSE_SUBTRACT;
        case BlendOp::Min: return GL_MIN;
        case BlendOp::Max: return GL_MAX;
        default: return GL_FUNC_ADD;
    }
}

GLenum Gl(StencilOp o) {
    switch (o) {
        case StencilOp::Zero: return GL_ZERO;
        case StencilOp::Replace: return GL_REPLACE;
        case StencilOp::IncrSat: return GL_INCR;
        case StencilOp::DecrSat: return GL_DECR;
        case StencilOp::Invert: return GL_INVERT;
        case StencilOp::Incr: return GL_INCR_WRAP;
        case StencilOp::Decr: return GL_DECR_WRAP;
        default: return GL_KEEP;
    }
}

GLenum Gl(Address a) {
    switch (a) {
        case Address::Wrap: return GL_REPEAT;
        case Address::Mirror: return GL_MIRRORED_REPEAT;
        case Address::Border: return GL_CLAMP_TO_BORDER;
        default: return GL_CLAMP_TO_EDGE;
    }
}

GLenum Gl(Topology t) {
    switch (t) {
        case Topology::PointList: return GL_POINTS;
        case Topology::LineList: return GL_LINES;
        case Topology::LineStrip: return GL_LINE_STRIP;
        case Topology::TriangleStrip: return GL_TRIANGLE_STRIP;
        default: return GL_TRIANGLES;
    }
}

void APIENTRY DebugCallback(GLenum, GLenum type, GLuint, GLenum severity, GLsizei, const GLchar* msg, const void*) {
    if (severity == GL_DEBUG_SEVERITY_NOTIFICATION) return;
    static int n = 0;
    if (n++ < 100) Log("GL debug (type %x): %s", type, msg);
}

GLuint Framebuffer(GlTexture* color, GlTexture* depth) {
    GLuint c = color ? color->name : 0, d = depth ? depth->name : 0;
    GLuint& fbo = g_fbos[{c, d}];
    if (!fbo) {
        g.glCreateFramebuffers(1, &fbo);
        if (c) g.glNamedFramebufferTexture(fbo, GL_COLOR_ATTACHMENT0, c, 0);
        if (d) g.glNamedFramebufferTexture(fbo, GL_DEPTH_STENCIL_ATTACHMENT, d, 0);
        GLenum st = g.glCheckNamedFramebufferStatus(fbo, GL_FRAMEBUFFER);
        if (st != GL_FRAMEBUFFER_COMPLETE) Log("OpenGL: framebuffer (%u, %u) incomplete: %x", c, d, st);
    }
    return fbo;
}

void ForgetTexture(GLuint name) {
    for (auto it = g_fbos.begin(); it != g_fbos.end();) {
        if (it->first.first == name || it->first.second == name) {
            g.glDeleteFramebuffers(1, &it->second);
            if (g_applied.fbo == it->second) g_applied.fbo = ~0u;
            it = g_fbos.erase(it);
        } else {
            ++it;
        }
    }
    for (GLuint& u : g_applied.units)
        if (u == name) u = ~0u;
}

GLuint Program(GlShader* vs, GlShader* ps) {
    auto key = std::make_pair(vs->name, ps->name);
    auto it = g_programs.find(key);
    if (it != g_programs.end()) return it->second;
    GLuint p = g.glCreateProgram();
    g.glAttachShader(p, vs->name);
    g.glAttachShader(p, ps->name);
    g.glLinkProgram(p);
    GLint ok = 0;
    g.glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096] = {};
        g.glGetProgramInfoLog(p, sizeof log, nullptr, log);
        Log("OpenGL: program link failed: %s", log);
        p = 0;
    }
    g_programs[key] = p;
    return p;
}

GLuint Sampler(const SamplerState& s) {
    std::string key(reinterpret_cast<const char*>(&s), sizeof s);
    GLuint& smp = g_samplers[key];
    if (smp) return smp;
    g.glCreateSamplers(1, &smp);
    GLenum min = s.min_linear ? (s.mip_linear ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_NEAREST)
                              : (s.mip_linear ? GL_NEAREST_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST);
    if (s.anisotropic) min = GL_LINEAR_MIPMAP_LINEAR;
    g.glSamplerParameteri(smp, GL_TEXTURE_MIN_FILTER, min);
    g.glSamplerParameteri(smp, GL_TEXTURE_MAG_FILTER, s.mag_linear || s.anisotropic ? GL_LINEAR : GL_NEAREST);
    g.glSamplerParameteri(smp, GL_TEXTURE_WRAP_S, Gl(s.u));
    g.glSamplerParameteri(smp, GL_TEXTURE_WRAP_T, Gl(s.v));
    g.glSamplerParameteri(smp, GL_TEXTURE_WRAP_R, Gl(s.w));
    g.glSamplerParameterfv(smp, GL_TEXTURE_BORDER_COLOR, s.border);
    g.glSamplerParameterf(smp, GL_TEXTURE_LOD_BIAS, s.lod_bias);
    g.glSamplerParameterf(smp, GL_TEXTURE_MIN_LOD, s.min_lod);
    g.glSamplerParameterf(smp, GL_TEXTURE_MAX_LOD, s.max_lod);
    g.glSamplerParameterf(smp, GL_TEXTURE_MAX_ANISOTROPY, s.anisotropic ? float(s.max_anisotropy) : 1.f);
    return smp;
}

void SetVertexLayout(GlShader* vs) {
    if (g_applied.layout_vs == vs) return;
    g_applied.layout_vs = vs;
    for (GLuint i = 0; i < 16; i++) g.glDisableVertexAttribArray(i);
    for (const VertexElement& e : vs->inputs) {
        GLuint loc = e.reg;
        g.glEnableVertexAttribArray(loc);
        g.glVertexAttribBinding(loc, e.stream);
        switch (e.format) {
            case VertexFormat::Float1: g.glVertexAttribFormat(loc, 1, GL_FLOAT, GL_FALSE, e.offset); break;
            case VertexFormat::Float2: g.glVertexAttribFormat(loc, 2, GL_FLOAT, GL_FALSE, e.offset); break;
            case VertexFormat::Float3: g.glVertexAttribFormat(loc, 3, GL_FLOAT, GL_FALSE, e.offset); break;
            case VertexFormat::Float4: g.glVertexAttribFormat(loc, 4, GL_FLOAT, GL_FALSE, e.offset); break;
            case VertexFormat::Color: g.glVertexAttribFormat(loc, GL_BGRA, GL_UNSIGNED_BYTE, GL_TRUE, e.offset); break;
            case VertexFormat::UByte4N: g.glVertexAttribFormat(loc, 4, GL_UNSIGNED_BYTE, GL_TRUE, e.offset); break;
            case VertexFormat::Short2N: g.glVertexAttribFormat(loc, 2, GL_SHORT, GL_TRUE, e.offset); break;
            case VertexFormat::Short4N: g.glVertexAttribFormat(loc, 4, GL_SHORT, GL_TRUE, e.offset); break;
            case VertexFormat::Short2: g.glVertexAttribIFormat(loc, 2, GL_SHORT, e.offset); break;
            case VertexFormat::Short4: g.glVertexAttribIFormat(loc, 4, GL_SHORT, e.offset); break;
        }
    }
}

void BindFramebuffer(GLuint fbo) {
    if (g_applied.fbo != fbo) g.glBindFramebuffer(GL_FRAMEBUFFER, fbo), g_applied.fbo = fbo;
}

void ApplyBlend(const BlendState& s, bool force) {
    if (!force && !memcmp(&s, &g_applied.blend, sizeof s)) return;
    g_applied.blend = s;
    if (s.enable)
        g.glEnable(GL_BLEND);
    else
        g.glDisable(GL_BLEND);
    g.glBlendFuncSeparate(Gl(s.src), Gl(s.dst), Gl(s.src_alpha), Gl(s.dst_alpha));
    g.glBlendEquation(Gl(s.op));
    g.glBlendColor(s.factor[0], s.factor[1], s.factor[2], s.factor[3]);
    g.glColorMask((s.write_mask & 1) != 0, (s.write_mask & 2) != 0, (s.write_mask & 4) != 0, (s.write_mask & 8) != 0);
}

void ApplyDepth(const DepthStencilState& s, bool force) {
    if (!force && !memcmp(&s, &g_applied.depth, sizeof s)) return;
    g_applied.depth = s;
    if (s.depth_enable)
        g.glEnable(GL_DEPTH_TEST);
    else
        g.glDisable(GL_DEPTH_TEST);
    g.glDepthMask(s.depth_write ? GL_TRUE : GL_FALSE);
    g.glDepthFunc(Gl(s.depth_func));
    if (s.stencil_enable)
        g.glEnable(GL_STENCIL_TEST);
    else
        g.glDisable(GL_STENCIL_TEST);
    g.glStencilFunc(Gl(s.stencil_func), s.stencil_ref, s.stencil_read_mask);
    g.glStencilOp(Gl(s.fail), Gl(s.depth_fail), Gl(s.pass));
    g.glStencilMask(s.stencil_write_mask);
}

void ApplyRaster(const RasterState& s, bool force) {
    if (!force && !memcmp(&s, &g_applied.raster, sizeof s)) return;
    g_applied.raster = s;
    g.glPolygonMode(GL_FRONT_AND_BACK, s.wireframe ? GL_LINE : GL_FILL);
    if (s.cull_back) {
        g.glEnable(GL_CULL_FACE);
        g.glCullFace(GL_BACK);
        // Row 0 is the top (see glClipControl), so windings in render target
        // pixels read the same as in D3D.
        g.glFrontFace(s.front_ccw ? GL_CCW : GL_CW);
    } else {
        g.glDisable(GL_CULL_FACE);
    }
    if (s.depth_bias || s.slope_bias) {
        g.glEnable(GL_POLYGON_OFFSET_FILL), g.glEnable(GL_POLYGON_OFFSET_LINE), g.glEnable(GL_POLYGON_OFFSET_POINT);
        g.glPolygonOffset(s.slope_bias, float(s.depth_bias));
    } else {
        g.glDisable(GL_POLYGON_OFFSET_FILL), g.glDisable(GL_POLYGON_OFFSET_LINE), g.glDisable(GL_POLYGON_OFFSET_POINT);
    }
}

void ApplyViewport(const Viewport& v, bool force) {
    if (!force && !memcmp(&v, &g_applied.vp, sizeof v)) return;
    g_applied.vp = v;
    g.glViewportIndexedf(0, v.x, v.y, v.w, v.h);
    g.glDepthRangeIndexed(0, v.min_z, v.max_z);
}

void ExecDraw(const Cmd& c) {
    const DrawState& s = c.state;
    if (!s.vs || !s.ps || !s.vs->name || !s.ps->name || (!s.rt && !s.ds)) return;
    GLuint program = Program(s.vs, s.ps);
    if (!program) return;
    bool force = !g_applied.valid;
    g_applied.valid = true;
    BindFramebuffer(Framebuffer(s.rt, s.ds));
    ApplyViewport(s.vp, force);
    ApplyBlend(s.blend, force);
    ApplyDepth(s.depth, force);
    ApplyRaster(s.raster, force);
    if (g_applied.program != program) g.glUseProgram(program), g_applied.program = program;
    for (int i = 0; i < 4; i++) {
        GlTexture* t = s.tex[i];
        if (t && (t == s.rt || t == s.ds)) t = nullptr;  // a bound target reads as null (as in D3D11)
        GLuint name = t ? t->name : 0;
        if (g_applied.units[i] != name) {
            g_applied.units[i] = name;
            if (name) {
                g.glBindTextureUnit(i, name);
            } else {
                g.glBindTextureUnit(i, g_dummy2d);
                g.glBindTextureUnit(i, g_dummycube);
            }
        }
        GLuint smp = Sampler(s.samp[i]);
        if (g_applied.samplers[i] != smp) g.glBindSampler(i, smp), g_applied.samplers[i] = smp;
    }
    for (int stage = 0; stage < 2; stage++)
        for (int slot = 0; slot < 4; slot++) {
            const Range& r = s.consts[stage][slot];
            if (r.bytes) g.glBindBufferRange(GL_UNIFORM_BUFFER, stage * 4 + slot, g_stream, r.offset, (r.bytes + 15) & ~15u);
        }
    SetVertexLayout(s.vs);
    for (GLuint n = 0; n < 16; n++) {
        const StreamBinding& b = s.streams[n];
        if (b.bytes) g.glBindVertexBuffer(n, g_stream, b.offset, b.stride);
    }
    if (c.indexed)
        g.glDrawElements(Gl(c.topo), GLsizei(c.count), GL_UNSIGNED_SHORT, reinterpret_cast<const void*>(uintptr_t(c.index_offset)));
    else
        g.glDrawArrays(Gl(c.topo), 0, GLsizei(c.count));
}

void ExecClear(const Cmd& c, const uint8_t* arena) {
    const DrawState& s = c.state;
    if (!s.rt && !s.ds) return;
    GLuint fbo = Framebuffer(s.rt, s.ds);
    // Clears obey masks and the scissor: open the masks (re-applied by the next draw).
    g.glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    g.glDepthMask(GL_TRUE);
    g.glStencilMask(0xFF);
    g_applied.valid = false;
    auto clear = [&] {
        if (c.has_color && s.rt) g.glClearNamedFramebufferfv(fbo, GL_COLOR, 0, c.color);
        if (s.ds) {
            if (c.has_depth && c.has_stencil) {
                g.glClearNamedFramebufferfi(fbo, GL_DEPTH_STENCIL, 0, c.z, c.stencil);
            } else if (c.has_depth) {
                g.glClearNamedFramebufferfv(fbo, GL_DEPTH, 0, &c.z);
            } else if (c.has_stencil) {
                GLint v = c.stencil;
                g.glClearNamedFramebufferiv(fbo, GL_STENCIL, 0, &v);
            }
        }
    };
    if (!c.rect_count) {
        clear();
        return;
    }
    const auto* rects = reinterpret_cast<const Rect*>(arena + c.rect_offset);
    g.glEnable(GL_SCISSOR_TEST);
    for (uint32_t i = 0; i < c.rect_count; i++) {
        const Rect& r = rects[i];
        g.glScissor(r.left, r.top, r.right - r.left, r.bottom - r.top);
        clear();
    }
    g.glDisable(GL_SCISSOR_TEST);
}

void Execute(Batch* b) {
    if (!b->arena.empty()) {
        g_stream = g_stream_buffers[g_stream_next++ & 3];
        g.glNamedBufferData(g_stream, GLsizeiptr(b->arena.size()), b->arena.data(), GL_STREAM_DRAW);
        g.glVertexArrayElementBuffer(g_vao, g_stream);
    }
    for (Cmd& c : b->cmds) {
        switch (c.kind) {
            case Kind::Draw: ExecDraw(c); break;
            case Kind::Clear: ExecClear(c, b->arena.data()); break;
            case Kind::Fn: c.fn(); break;
        }
    }
    b->cmds.clear();
    b->arena.clear();
}

bool InitContext() {
    SDL_Window* w = platform::Window();
    if (!w) {
        Log("OpenGL: no window");
        return false;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 5);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    static const bool debug = getenv("KT_GL_DEBUG") != nullptr;
    if (debug) SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);
    g_context = SDL_GL_CreateContext(w);
    if (!g_context) {
        Log("OpenGL: 4.5 core context unavailable: %s", SDL_GetError());
        return false;
    }
    SDL_GL_MakeCurrent(w, g_context);
    SDL_GL_SetSwapInterval(0);  // frame pacing is the caller's
    if (!LoadApi()) return false;
    Log("OpenGL: %s / %s", reinterpret_cast<const char*>(g.glGetString(GL_VERSION)),
        reinterpret_cast<const char*>(g.glGetString(GL_RENDERER)));
    if (debug && g.glDebugMessageCallback) {
        g.glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        g.glDebugMessageCallback(DebugCallback, nullptr);
    }
    g.glClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE);
    g.glEnable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
    g.glCreateVertexArrays(1, &g_vao);
    g.glBindVertexArray(g_vao);
    g.glCreateBuffers(4, g_stream_buffers);
    GLint align = 256;
    g.glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &align);
    g_ubo_align = uint32_t(std::max(align, 16));
    // Unbound texture units sample (0, 0, 0, 0), as in D3D11.
    const uint8_t zero[4] = {};
    g.glCreateTextures(GL_TEXTURE_2D, 1, &g_dummy2d);
    g.glTextureStorage2D(g_dummy2d, 1, GL_RGBA8, 1, 1);
    g.glTextureSubImage2D(g_dummy2d, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, zero);
    g.glCreateTextures(GL_TEXTURE_CUBE_MAP, 1, &g_dummycube);
    g.glTextureStorage2D(g_dummycube, 1, GL_RGBA8, 1, 1);
    for (int f = 0; f < 6; f++) g.glTextureSubImage3D(g_dummycube, 0, 0, 0, f, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, zero);
    return true;
}

const char* const kGlslPrelude = R"(#version 450 core
#define float2 vec2
#define float3 vec3
#define float4 vec4
#define float3x3 mat3
#define float4x4 mat4
#define int2 ivec2
#define int3 ivec3
#define int4 ivec4
#define lerp mix
#define frac fract
#define rsqrt inversesqrt
#define saturate(x) clamp(x, 0.0, 1.0)
#define mul(a, b) ((a) * (b))
#define UNIFORMS_VS(name, slot) layout(std140, binding = slot) uniform name
#define UNIFORMS_PS(name, slot) layout(std140, binding = 4 + slot) uniform name
#define ROW_MAJOR layout(row_major)
#define TEXTURE2D(name, slot) layout(binding = slot) uniform sampler2D name;
#define TEXTURECUBE(name, slot) layout(binding = slot) uniform samplerCube name;
#define SAMPLE(name, coord) texture(name, coord)
#define LOAD2D(name, xy) texelFetch(name, ivec2(xy), 0)
#define TO3X3(m) mat3(m)
#define XXXX(s) vec4(s)
#define XXX(s) vec3(s)
#define UNROLL
vec4 SLT(vec4 a, vec4 b) { return vec4(lessThan(a, b)); }
vec4 SGE(vec4 a, vec4 b) { return vec4(greaterThanEqual(a, b)); }
vec4 lit(float nl, float nh, float m) {
    return vec4(1.0, max(nl, 0.0), (nl < 0.0 || nh < 0.0) ? 0.0 : pow(nh, m), 1.0);
}
struct VSOut { vec4 pos; vec4 d0; vec4 d1; vec4 t0; vec4 t1; vec4 t2; vec4 t3; float fog; };
)";

const char* const kVaryingsOut = R"(
layout(location = 0) out vec4 v_d0;
layout(location = 1) out vec4 v_d1;
layout(location = 2) out vec4 v_t0;
layout(location = 3) out vec4 v_t1;
layout(location = 4) out vec4 v_t2;
layout(location = 5) out vec4 v_t3;
layout(location = 6) out float v_fog;
)";

const char* const kPixelMain = R"(
layout(location = 0) in vec4 v_d0;
layout(location = 1) in vec4 v_d1;
layout(location = 2) in vec4 v_t0;
layout(location = 3) in vec4 v_t1;
layout(location = 4) in vec4 v_t2;
layout(location = 5) in vec4 v_t3;
layout(location = 6) in float v_fog;
layout(location = 0) out vec4 frag_color;
void main() {
    VSOut i;
    i.pos = gl_FragCoord;
    i.d0 = v_d0; i.d1 = v_d1; i.t0 = v_t0; i.t1 = v_t1; i.t2 = v_t2; i.t3 = v_t3; i.fog = v_fog;
    frag_color = XboxPS(i);
}
)";

std::string VertexMain(const std::vector<VertexElement>& inputs) {
    std::string s = kVaryingsOut;
    char buf[128];
    for (const VertexElement& e : inputs) {
        snprintf(buf, sizeof buf, "layout(location = %u) in %s in_r%u;\n", e.reg, IsInteger(e.format) ? "ivec4" : "vec4", e.reg);
        s += buf;
    }
    s += "void main() {\n    vec4 v[16];\n    for (int k = 0; k < 16; k++) v[k] = vec4(0, 0, 0, 1);\n";
    for (const VertexElement& e : inputs) {
        snprintf(buf, sizeof buf, "    v[%u] = vec4(in_r%u);\n", e.reg, e.reg);
        s += buf;
    }
    s += R"(    VSOut o;
    XboxVS(v, gl_VertexID, o);
    gl_Position = o.pos;
    v_d0 = o.d0; v_d1 = o.d1; v_t0 = o.t0; v_t1 = o.t1; v_t2 = o.t2; v_t3 = o.t3; v_fog = o.fog;
}
)";
    return s;
}

// Render thread: compiles one stage; 0 on failure (logged with the source).
GLuint CompileStage(GLenum type, const std::string& src, const char* tag) {
    GLuint sh = g.glCreateShader(type);
    const char* p = src.c_str();
    g.glShaderSource(sh, 1, &p, nullptr);
    g.glCompileShader(sh);
    GLint ok = 0;
    g.glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096] = {};
        g.glGetShaderInfoLog(sh, sizeof log, nullptr, log);
        Log("%s: GLSL compile failed: %s\n---\n%s\n---", tag, log, src.c_str());
        g.glDeleteShader(sh);
        return 0;
    }
    return sh;
}

GLenum TextureFormat(Format f) {
    switch (f) {
        case Format::BGRA8: return GL_RGBA8;
        case Format::BC1: return 0x83F1;  // COMPRESSED_RGBA_S3TC_DXT1_EXT
        case Format::BC2: return 0x83F2;  // COMPRESSED_RGBA_S3TC_DXT3_EXT
        case Format::BC3: return 0x83F3;  // COMPRESSED_RGBA_S3TC_DXT5_EXT
        case Format::D24S8: return GL_DEPTH24_STENCIL8;
    }
    return GL_RGBA8;
}

bool Compressed(Format f) { return f == Format::BC1 || f == Format::BC2 || f == Format::BC3; }

}  // namespace

// ---- Backend entry points -------------------------------------------------------------------------

namespace {

std::mutex g_init_mu;
std::condition_variable g_init_cv;
int g_init_state = 0;  // 0 pending, 1 ok, -1 failed

void RenderLoop() {
    bool ok = InitContext();
    {
        std::lock_guard<std::mutex> lk(g_init_mu);
        g_init_state = ok ? 1 : -1;
    }
    g_init_cv.notify_all();
    if (!ok) return;
    for (;;) {
        Batch* b;
        {
            std::unique_lock<std::mutex> lk(g_mu);
            g_cv.wait(lk, [] { return !g_queue.empty(); });
            b = g_queue.front();
            g_queue.pop_front();
        }
        Execute(b);
        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_free.push_back(b);
            g_completed++;
        }
        g_cv.notify_all();
    }
}

}  // namespace

bool CreateDevice() {
    std::thread(RenderLoop).detach();
    std::unique_lock<std::mutex> lk(g_init_mu);
    g_init_cv.wait(lk, [] { return g_init_state != 0; });
    return g_init_state > 0;
}

Texture* CreateTexture(uint32_t w, uint32_t h, uint32_t levels, Format f, bool cube, const SubresourceData* init) {
    auto* t = new GlTexture;
    t->w = w, t->h = h, t->format = f, t->cube = cube;
    uint32_t faces = cube ? 6 : 1;
    // Copy the data: it is uploaded later on the render thread.
    auto data = std::make_shared<std::vector<std::vector<uint8_t>>>(faces * levels);
    std::vector<uint32_t> pitches(faces * levels);
    for (uint32_t fc = 0; fc < faces; fc++)
        for (uint32_t l = 0; l < levels; l++) {
            uint32_t i = fc * levels + l, lh = std::max(h >> l, 1u);
            uint32_t rows = Compressed(f) ? (lh + 3) / 4 : lh;
            const auto* p = static_cast<const uint8_t*>(init[i].data);
            (*data)[i].assign(p, p + size_t(init[i].row_pitch) * rows);
            pitches[i] = init[i].row_pitch;
        }
    PushFn([t, levels, faces, data, pitches] {
        Format f = t->format;
        g.glCreateTextures(t->cube ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D, 1, &t->name);
        g.glTextureStorage2D(t->name, levels, TextureFormat(f), t->w, t->h);
        g.glTextureParameteri(t->name, GL_TEXTURE_MAX_LEVEL, levels - 1);
        for (uint32_t fc = 0; fc < faces; fc++)
            for (uint32_t l = 0; l < levels; l++) {
                uint32_t i = fc * levels + l;
                uint32_t lw = std::max(t->w >> l, 1u), lh = std::max(t->h >> l, 1u);
                const auto& d = (*data)[i];
                if (Compressed(f)) {
                    if (t->cube)
                        g.glCompressedTextureSubImage3D(t->name, l, 0, 0, fc, lw, lh, 1, TextureFormat(f), GLsizei(d.size()), d.data());
                    else
                        g.glCompressedTextureSubImage2D(t->name, l, 0, 0, lw, lh, TextureFormat(f), GLsizei(d.size()), d.data());
                } else {
                    g.glPixelStorei(GL_UNPACK_ROW_LENGTH, pitches[i] / 4);
                    if (t->cube)
                        g.glTextureSubImage3D(t->name, l, 0, 0, fc, lw, lh, 1, GL_BGRA, GL_UNSIGNED_BYTE, d.data());
                    else
                        g.glTextureSubImage2D(t->name, l, 0, 0, lw, lh, GL_BGRA, GL_UNSIGNED_BYTE, d.data());
                    g.glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                }
            }
    });
    return t;
}

Texture* CreateRenderTarget(uint32_t w, uint32_t h, bool depth) {
    auto* t = new GlTexture;
    t->w = w, t->h = h, t->format = depth ? Format::D24S8 : Format::BGRA8, t->target = true;
    PushFn([t] {
        g.glCreateTextures(GL_TEXTURE_2D, 1, &t->name);
        g.glTextureStorage2D(t->name, 1, TextureFormat(t->format), t->w, t->h);
        g.glTextureParameteri(t->name, GL_TEXTURE_MAX_LEVEL, 0);
    });
    return t;
}

void DestroyTexture(Texture* base) {
    if (!base) return;
    GlTexture* t = Gl(base);
    for (auto*& b : g_state.tex)
        if (b == t) b = nullptr;
    if (g_state.rt == t) g_state.rt = nullptr;
    if (g_state.ds == t) g_state.ds = nullptr;
    PushFn([t] {
        ForgetTexture(t->name);
        g.glDeleteTextures(1, &t->name);
        delete t;
    });
}

void UpdateTexture(Texture* base, const Rect& r, const void* bgra, uint32_t pitch) {
    GlTexture* t = Gl(base);
    int w = r.right - r.left, h = r.bottom - r.top;
    auto data = std::make_shared<std::vector<uint8_t>>(size_t(w) * h * 4);
    for (int y = 0; y < h; y++) memcpy(data->data() + size_t(y) * w * 4, static_cast<const uint8_t*>(bgra) + size_t(y) * pitch, w * 4);
    PushFn([t, r, w, h, data] {
        g.glTextureSubImage2D(t->name, 0, r.left, r.top, w, h, GL_BGRA, GL_UNSIGNED_BYTE, data->data());
    });
}

void CopyTexture(Texture* dst, int dx, int dy, Texture* src, const Rect& r) {
    GlTexture *d = Gl(dst), *s = Gl(src);
    PushFn([d, s, dx, dy, r] {
        g.glCopyImageSubData(s->name, GL_TEXTURE_2D, 0, r.left, r.top, 0, d->name, GL_TEXTURE_2D, 0, dx, dy, 0,
                             r.right - r.left, r.bottom - r.top, 1);
    });
}

bool ReadTexture(Texture* src, const Rect& r, std::vector<uint8_t>* bgra) {
    GlTexture* s = Gl(src);
    int w = r.right - r.left, h = r.bottom - r.top;
    bgra->resize(size_t(w) * h * 4);
    RunSync([&] {
        g.glPixelStorei(GL_PACK_ALIGNMENT, 4);
        g.glGetTextureSubImage(s->name, 0, r.left, r.top, 0, w, h, 1, GL_BGRA, GL_UNSIGNED_BYTE, GLsizei(bgra->size()),
                               bgra->data());
    });
    return true;
}

Shader* CompileVertexShader(const std::string& src, const std::vector<VertexElement>& inputs, const char* tag) {
    static std::map<std::pair<std::string, std::string>, GlShader*> cache;
    std::string sig;
    for (const VertexElement& e : inputs) {
        char buf[48];
        snprintf(buf, sizeof buf, "%u:%u:%u:%d;", e.reg, e.stream, e.offset, int(e.format));
        sig += buf;
    }
    auto key = std::make_pair(src, sig);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    std::string full = kGlslPrelude + src + VertexMain(inputs);
    DumpShader(full, tag, "vert");
    GLuint name = 0;
    RunSync([&] { name = CompileStage(GL_VERTEX_SHADER, full, tag); });
    GlShader* s = nullptr;
    if (name) {
        s = new GlShader;
        s->name = name, s->vertex = true, s->inputs = inputs;
    }
    cache[key] = s;
    return s;
}

Shader* CompilePixelShader(const std::string& src, const char* tag) {
    static std::unordered_map<std::string, GlShader*> cache;
    auto it = cache.find(src);
    if (it != cache.end()) return it->second;
    std::string full = kGlslPrelude + src + kPixelMain;
    DumpShader(full, tag, "frag");
    GLuint name = 0;
    RunSync([&] { name = CompileStage(GL_FRAGMENT_SHADER, full, tag); });
    GlShader* s = nullptr;
    if (name) {
        s = new GlShader;
        s->name = name;
    }
    cache[src] = s;
    return s;
}

void SetRenderTargets(Texture* color, Texture* depth) { g_state.rt = Gl(color), g_state.ds = Gl(depth); }
void SetViewport(const Viewport& vp) { g_state.vp = vp; }
void SetBlendState(const BlendState& s) { g_state.blend = s; }
void SetDepthStencilState(const DepthStencilState& s) { g_state.depth = s; }
void SetRasterState(const RasterState& s) { g_state.raster = s; }

void SetTexture(uint32_t slot, Texture* t) {
    if (slot < 4) g_state.tex[slot] = Gl(t);
}

void SetSampler(uint32_t slot, const SamplerState& s) {
    if (slot < 4) g_state.samp[slot] = s;
}

void SetConstants(Stage stage, uint32_t slot, const void* data, uint32_t bytes) {
    if (slot >= 4) return;
    int st = stage == Stage::Vertex ? 0 : 1;
    std::vector<uint8_t>& last = g_last_consts[st][slot];
    if (last.size() == bytes && !memcmp(last.data(), data, bytes)) {
        g_state.consts[st][slot] = g_last_range[st][slot];
        return;
    }
    Range r{ArenaPut(data, bytes, g_ubo_align, (bytes + 15) & ~15u), bytes};
    last.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + bytes);
    g_last_range[st][slot] = r;
    g_state.consts[st][slot] = r;
}

void SetShaders(Shader* vs, Shader* ps) { g_state.vs = Gl(vs), g_state.ps = Gl(ps); }

void SetVertexStream(uint32_t stream, const void* data, uint32_t bytes, uint32_t stride) {
    if (stream >= 16) return;
    if (!data || !bytes) {
        g_state.streams[stream] = {};
        return;
    }
    g_state.streams[stream] = {ArenaPut(data, bytes, 16), bytes, stride};
}

void Draw(Topology t, uint32_t vertices) {
    Cmd& c = Push(Kind::Draw);
    c.topo = t, c.count = vertices, c.state = g_state;
    MaybeSubmit();
}

void DrawIndexed(Topology t, const uint16_t* indices, uint32_t count) {
    uint32_t off = ArenaPut(indices, count * 2, 4);
    Cmd& c = Push(Kind::Draw);
    c.topo = t, c.count = count, c.indexed = true, c.index_offset = off, c.state = g_state;
    MaybeSubmit();
}

void Clear(const Rect* rects, uint32_t count, const float* color, const float* depth, const uint8_t* stencil) {
    uint32_t off = rects && count ? ArenaPut(rects, count * sizeof(Rect), 4) : 0;
    Cmd& c = Push(Kind::Clear);
    c.state = g_state;
    c.rect_offset = off, c.rect_count = rects ? count : 0;
    if (color) c.has_color = true, memcpy(c.color, color, 16);
    if (depth) c.has_depth = true, c.z = *depth;
    if (stencil) c.has_stencil = true, c.stencil = *stencil;
    MaybeSubmit();
}

void BlitTexture(Texture* src, Texture* dst) {
    GlTexture *s = Gl(src), *d = Gl(dst);
    PushFn([s, d] {
        if (s->w == d->w && s->h == d->h) {
            g.glCopyImageSubData(s->name, GL_TEXTURE_2D, 0, 0, 0, 0, d->name, GL_TEXTURE_2D, 0, 0, 0, 0, s->w, s->h, 1);
            return;
        }
        g.glBlitNamedFramebuffer(Framebuffer(s, nullptr), Framebuffer(d, nullptr), 0, 0, s->w, s->h, 0, 0, d->w, d->h,
                                 GL_COLOR_BUFFER_BIT, GL_LINEAR);
    });
}

void Present(Texture* image) {
    GlTexture* t = Gl(image);
    uint64_t n = ++g_present_requests;
    PushFn([t] {
        SDL_Window* w = platform::Window();
        int ww = 0, wh = 0;
        SDL_GetWindowSizeInPixels(w, &ww, &wh);
        BindFramebuffer(0);
        g.glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        g_applied.valid = false;
        g.glClearColor(0, 0, 0, 1);
        g.glClear(GL_COLOR_BUFFER_BIT);
        if (ww > 0 && wh > 0) {
            float s = std::min(float(ww) / t->w, float(wh) / t->h);
            int w2 = int(t->w * s), h2 = int(t->h * s);
            int x0 = (ww - w2) / 2, top = (wh - h2) / 2;
            // Image row 0 is its top; the window's row 0 is the bottom.
            g.glBlitNamedFramebuffer(Framebuffer(t, nullptr), 0, 0, 0, t->w, t->h, x0, wh - top, x0 + w2, wh - top - h2,
                                     GL_COLOR_BUFFER_BIT, GL_LINEAR);
        }
        SDL_GL_SwapWindow(w);
        g_presented++;
    });
    Submit();
    // Keep at most one frame queued behind the one being drawn.
    std::unique_lock<std::mutex> lk(g_mu);
    g_cv.wait(lk, [&] { return g_presented + 1 >= n; });
}

}  // namespace gl
}  // namespace gpu
