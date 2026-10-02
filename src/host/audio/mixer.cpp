// Software voice mixer: linear-interpolation resampling into a 48 kHz
// stereo float mix, pulled by the SDL audio device or a real-time null sink.
#include "mixer.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "../host.h"

namespace audio {

namespace {

constexpr int kRate = 48000;

}  // namespace

struct Voice {
    int channels = 1, bits = 16, rate = kRate;
    BufferEndFn on_end = nullptr;
    void* owner = nullptr;
    std::deque<VoiceBuffer> queue;
    double pos = 0;     // frame position in the head buffer
    double played = 0;  // source frames consumed
    bool running = false;
    float volume = 1;
    double ratio = 1;
    bool started_head = false;  // pos initialised from the head's play_begin

    float Sample(const VoiceBuffer& b, uint32_t frame, int ch) const {
        ch = std::min(ch, channels - 1);
        size_t i = size_t(frame) * channels + ch;
        if (bits == 8) return (static_cast<const uint8_t*>(b.data)[i] - 128) * (1.f / 128);
        return static_cast<const int16_t*>(b.data)[i] * (1.f / 32768);
    }
};

namespace {

std::mutex g_mu;
std::vector<Voice*> g_voices;
const char* g_sink = "silent";
bool g_inited = false;

struct Completion {
    BufferEndFn fn;
    void* owner;
    uint64_t context;
};

uint32_t LoopEnd(const VoiceBuffer& b) {
    if (!b.loop || !b.loop_length) return b.frames;
    return std::min(b.frames, b.loop_begin + b.loop_length);
}

// Adds `frames` output frames of `v` into `out` (stereo); g_mu held.
void MixVoice(Voice* v, float* out, int frames, std::vector<Completion>* done) {
    double step = double(v->rate) * v->ratio / kRate;
    for (int n = 0; n < frames; n++) {
        if (v->queue.empty()) return;
        VoiceBuffer* b = &v->queue.front();
        if (!v->started_head) v->pos = std::min(b->play_begin, b->frames), v->started_head = true;
        uint32_t end = LoopEnd(*b);
        while (v->pos >= end) {
            if (b->loop && end > b->loop_begin) {
                v->pos = b->loop_begin + std::fmod(v->pos - end, double(end - b->loop_begin));
                break;
            }
            done->push_back({v->on_end, v->owner, b->context});
            double over = v->pos - end;
            v->queue.pop_front();
            if (v->queue.empty()) {
                v->started_head = false;
                return;
            }
            b = &v->queue.front();
            v->pos = std::min(b->play_begin, b->frames) + over;
            end = LoopEnd(*b);
        }
        uint32_t i = uint32_t(v->pos);
        float f = float(v->pos - i);
        uint32_t j = i + 1 < end ? i + 1 : (b->loop ? b->loop_begin : i);
        for (int c = 0; c < 2; c++) {
            float a = v->Sample(*b, i, c), z = v->Sample(*b, j, c);
            out[2 * n + c] += (a + (z - a) * f) * v->volume;
        }
        v->pos += step;
        v->played += step;
    }
}

// Produces `frames` frames of the mix, then reports finished buffers.
void Mix(float* out, int frames) {
    memset(out, 0, sizeof(float) * 2 * frames);
    std::vector<Completion> done;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        for (Voice* v : g_voices)
            if (v->running) MixVoice(v, out, frames, &done);
    }
    for (int i = 0; i < 2 * frames; i++) out[i] = std::clamp(out[i], -1.f, 1.f);
    for (const Completion& c : done)
        if (c.fn) c.fn(c.owner, c.context);
}

void SDLCALL Feed(void*, SDL_AudioStream* stream, int additional, int) {
    static std::vector<float> buf;
    int frames = additional / int(sizeof(float) * 2);
    if (frames <= 0) return;
    buf.resize(size_t(frames) * 2);
    Mix(buf.data(), frames);
    SDL_PutAudioStreamData(stream, buf.data(), frames * int(sizeof(float) * 2));
}

// No device: consume the mix at the device rate.
void NullSink() {
    using Clock = std::chrono::steady_clock;
    std::vector<float> buf;
    auto start = Clock::now();
    uint64_t mixed = 0;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        uint64_t due = uint64_t(std::chrono::duration<double>(Clock::now() - start).count() * kRate);
        if (due - mixed > kRate / 10) mixed = due - kRate / 10;  // after a stall, drop the backlog
        int frames = int(due - mixed);
        if (frames <= 0) continue;
        buf.resize(size_t(frames) * 2);
        Mix(buf.data(), frames);
        mixed += frames;
    }
}

}  // namespace

void Init() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_inited) return;
    g_inited = true;
    if (!getenv("KT_SILENT") && SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        SDL_AudioSpec spec{SDL_AUDIO_F32, 2, kRate};
        SDL_AudioStream* s = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, Feed, nullptr);
        if (s && SDL_ResumeAudioStreamDevice(s)) {
            static char name[64];
            snprintf(name, sizeof name, "SDL (%s)", SDL_GetCurrentAudioDriver());
            g_sink = name;
            Log("audio: output on %s", g_sink);
            return;
        }
        Log("audio: no output device (%s); mixing silently in real time", SDL_GetError());
    } else {
        Log("audio: silent mode; mixing in real time");
    }
    std::thread(NullSink).detach();
}

const char* SinkName() { return g_sink; }

Voice* CreateVoice(int channels, int bits, int rate, BufferEndFn on_end, void* owner) {
    auto* v = new Voice;
    v->channels = std::max(channels, 1), v->bits = bits == 8 ? 8 : 16, v->rate = rate > 0 ? rate : kRate;
    v->on_end = on_end, v->owner = owner;
    std::lock_guard<std::mutex> lk(g_mu);
    g_voices.push_back(v);
    return v;
}

void DestroyVoice(Voice* v) {
    if (!v) return;
    std::lock_guard<std::mutex> lk(g_mu);
    g_voices.erase(std::remove(g_voices.begin(), g_voices.end(), v), g_voices.end());
    delete v;
}

bool Submit(Voice* v, const VoiceBuffer& b) {
    if (!v || !b.data || !b.frames) return false;
    std::lock_guard<std::mutex> lk(g_mu);
    v->queue.push_back(b);
    return true;
}

void Flush(Voice* v) {
    std::lock_guard<std::mutex> lk(g_mu);
    v->queue.clear();
    v->started_head = false;
}

void Start(Voice* v) {
    std::lock_guard<std::mutex> lk(g_mu);
    v->running = true;
}

void Stop(Voice* v) {
    std::lock_guard<std::mutex> lk(g_mu);
    v->running = false;
}

void SetVolume(Voice* v, float amplitude) {
    std::lock_guard<std::mutex> lk(g_mu);
    v->volume = amplitude;
}

void SetFrequencyRatio(Voice* v, double ratio) {
    std::lock_guard<std::mutex> lk(g_mu);
    v->ratio = ratio;
}

uint64_t FramesPlayed(Voice* v) {
    std::lock_guard<std::mutex> lk(g_mu);
    return uint64_t(v->played);
}

size_t BuffersQueued(Voice* v) {
    std::lock_guard<std::mutex> lk(g_mu);
    return v->queue.size();
}

}  // namespace audio
