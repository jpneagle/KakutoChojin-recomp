// DirectSound HLE on the host mixer (audio/mixer.h).
//
// Sound buffers and streams become mixer voices (Xbox ADPCM is decoded to
// PCM16 first). Without an audio device the mixer still runs in real time,
// so buffer cursors advance and stream packets complete at their playback
// rate, and titles that pace themselves on audio keep working.
#include "../hle.h"
#include "../host.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <deque>
#include <mutex>
#include <new>
#include <vector>

#include "../audio/mixer.h"
#include "../ob.h"
#include "adpcm.h"

namespace {

using Clock = std::chrono::steady_clock;

constexpr HRESULT kPending = HRESULT(0x8000000A);  // E_PENDING
constexpr HRESULT kFlushed = HRESULT(0x80004004);  // E_ABORT

#pragma pack(push, 1)
struct XWaveFormat {
    WORD wFormatTag, nChannels;
    DWORD nSamplesPerSec, nAvgBytesPerSec;
    WORD nBlockAlign, wBitsPerSample, cbSize;
};
#pragma pack(pop)

// Title structures: pointers are guest addresses.
struct XBufferDesc {  // DSBUFFERDESC
    DWORD dwSize, dwFlags, dwBufferBytes;
    GPtr<XWaveFormat> lpwfxFormat;
    uint32_t lpMixBins;
    DWORD dwInputMixBin;
};

struct XStreamDesc {  // DSSTREAMDESC
    DWORD dwFlags, dwMaxAttachedPackets;
    GPtr<XWaveFormat> lpwfxFormat;
    uint32_t lpfnCallback;  // void WINAPI (stream context, packet context, status)
    uint32_t lpvContext;
    uint32_t lpMixBins;
};

struct XMediaPacket {
    GPtr<uint8_t> pvBuffer;
    DWORD dwMaxSize;
    GPtr<DWORD> pdwCompletedSize;
    GPtr<DWORD> pdwStatus;
    uint32_t hCompletionEventOrContext;  // union { HANDLE hCompletionEvent; LPVOID pContext; }
    GPtr<LONGLONG> prtTimestamp;
};

struct MixBinVolumePair {
    DWORD dwMixBin;
    LONG lVolume;
};
struct MixBins {  // DSMIXBINS
    DWORD dwMixBinCount;
    GPtr<const MixBinVolumePair> pairs;
};

std::recursive_mutex g_mu;

bool IsAdpcm(const XWaveFormat& f) { return f.wFormatTag == adpcm::kFormatTag; }

// The format the mixer is fed: PCM (ADPCM is decoded to 16-bit).
struct HostFormat {
    WORD nChannels = 1, wBitsPerSample = 16, nBlockAlign = 2;
    DWORD nSamplesPerSec = 48000;
};

HostFormat HostFormatOf(const XWaveFormat& f) {
    HostFormat h;
    h.nChannels = std::max<WORD>(f.nChannels, 1);
    h.nSamplesPerSec = f.nSamplesPerSec ? f.nSamplesPerSec : 48000;
    h.wBitsPerSample = IsAdpcm(f) ? 16 : (f.wBitsPerSample == 8 ? 8 : 16);
    h.nBlockAlign = WORD(h.nChannels * h.wBitsPerSample / 8);
    return h;
}

audio::Voice* NewVoice(const HostFormat& h, audio::BufferEndFn on_end = nullptr, void* owner = nullptr) {
    return audio::CreateVoice(h.nChannels, h.wBitsPerSample, int(h.nSamplesPerSec), on_end, owner);
}

float Amplitude(LONG hundredths_db) { return hundredths_db <= -10000 ? 0.f : powf(10.f, hundredths_db / 2000.f); }

// The mixer calls back on its own thread; title callbacks need a fake KPCR.
void EnsureTitleThread() {
    thread_local bool adopted = false;
    if (!adopted) XThreadAdoptCurrent(), adopted = true;
}

// ---- Buffers -------------------------------------------------------------------------------

struct Buffer {
    LONG refs = 1;
    XWaveFormat format{};
    HostFormat host{};
    DWORD bytes = 0;
    const uint8_t* data = nullptr;
    std::vector<int16_t> decoded;  // ADPCM buffers
    const uint8_t* decoded_from = nullptr;
    DWORD loop_start = 0, loop_length = 0;  // bytes, in the title's format
    bool playing = false, looping = false;
    double frequency_ratio = 1;
    float volume = 1;
    audio::Voice* voice = nullptr;
    // Cursor bookkeeping, in samples of the decoded stream.
    uint32_t base_sample = 0;
    uint64_t played_at_start = 0;

    uint32_t TotalSamples() const {
        if (IsAdpcm(format)) return uint32_t(adpcm::BytesToSamples(bytes, host.nChannels));
        return host.nBlockAlign ? bytes / host.nBlockAlign : 0;
    }
    uint32_t ToSamples(DWORD b) const {
        return IsAdpcm(format) ? adpcm::BytesToSamples(b, host.nChannels) : (host.nBlockAlign ? b / host.nBlockAlign : 0);
    }
    DWORD ToBytes(uint32_t s) const {
        return IsAdpcm(format) ? adpcm::SamplesToBytes(s, host.nChannels) : s * host.nBlockAlign;
    }

    uint32_t PlayedSamples() { return voice ? uint32_t(audio::FramesPlayed(voice) - played_at_start) : 0; }

    // Current play position in samples; one-shot buffers stop at the end.
    uint32_t Cursor() {
        uint32_t total = TotalSamples();
        if (!playing || !total) return base_sample;
        uint64_t pos = uint64_t(base_sample) + PlayedSamples();
        if (looping) {
            uint32_t ls = loop_length ? ToSamples(loop_start) : 0;
            uint32_t ll = loop_length ? ToSamples(loop_length) : total;
            if (ll && pos >= ls + ll) pos = ls + (pos - ls) % ll;
            return uint32_t(pos);
        }
        if (pos >= total) {
            playing = false;
            base_sample = 0;
            return total - 1;
        }
        return uint32_t(pos);
    }

    void StartAt(uint32_t sample) {
        base_sample = sample;
        if (!data) return;
        if (IsAdpcm(format) && decoded_from != data) {
            decoded = adpcm::Decode(data, bytes, host.nChannels);
            decoded_from = data;
        }
        if (!voice) voice = NewVoice(host);
        audio::Stop(voice);
        audio::Flush(voice);
        uint32_t total = TotalSamples();
        if (!total) return;
        audio::VoiceBuffer vb;
        vb.data = IsAdpcm(format) ? static_cast<const void*>(decoded.data()) : data;
        vb.frames = IsAdpcm(format) ? uint32_t(decoded.size() / host.nChannels) : total;
        if (looping) {
            vb.loop = true;
            vb.loop_begin = loop_length ? ToSamples(loop_start) : 0;
            vb.loop_length = loop_length ? ToSamples(loop_length) : 0;
            if (vb.loop_length && sample >= vb.loop_begin + vb.loop_length) sample = vb.loop_begin;
        }
        vb.play_begin = std::min(sample, total - 1);
        base_sample = vb.play_begin;
        if (!audio::Submit(voice, vb)) return;
        audio::SetFrequencyRatio(voice, frequency_ratio);
        audio::SetVolume(voice, volume);
        played_at_start = audio::FramesPlayed(voice);
        audio::Start(voice);
    }

    void Halt() {
        base_sample = Cursor();
        playing = false;
        if (voice) audio::Stop(voice), audio::Flush(voice);
    }
};

// ---- Streams ---------------------------------------------------------------------------------

struct Stream;
// XMediaObject vtable in guest memory: AddRef, Release, GetInfo, GetStatus,
// Process, Discontinuity, Flush (title-callable addresses).
uint32_t StreamVtable();

struct Packet {
    XMediaPacket p;
    uint64_t id = 0;
    std::vector<int16_t> decoded;
};

void CompletePacket(Stream* s, const XMediaPacket& p, HRESULT status);

struct Stream {
    uint32_t vtbl = 0;  // the title's XMediaObject pointer points here
    LONG refs = 1;
    XStreamDesc desc{};
    XWaveFormat format{};
    HostFormat host{};
    DWORD max_packets = 0;
    std::deque<Packet*> queue;
    uint64_t next_id = 1;
    bool paused = false;
    float volume = 1;
    audio::Voice* voice = nullptr;
};

// Mixer callback: the packet with this id finished playing (unless it was
// flushed meanwhile).
void OnPacketEnd(void* owner, uint64_t id) {
    EnsureTitleThread();
    auto* s = static_cast<Stream*>(owner);
    Packet* pk = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lk(g_mu);
        auto it = std::find_if(s->queue.begin(), s->queue.end(), [&](Packet* q) { return q->id == id; });
        if (it == s->queue.end()) return;
        pk = *it;
        s->queue.erase(it);
    }
    CompletePacket(s, pk->p, S_OK);
    delete pk;
}

Stream* FromTitle(void* p) { return reinterpret_cast<Stream*>(static_cast<uint8_t*>(p) - offsetof(Stream, vtbl)); }

void CompletePacket(Stream* s, const XMediaPacket& p, HRESULT status) {
    if (p.pdwCompletedSize) *p.pdwCompletedSize = status == S_OK ? p.dwMaxSize : 0;
    if (p.pdwStatus) *p.pdwStatus = DWORD(status);
    if (s->desc.lpfnCallback)
        CallGuest(s->desc.lpfnCallback, {s->desc.lpvContext, p.hCompletionEventOrContext, DWORD(status)});
    else if (p.hCompletionEventOrContext)
        ob::SetEventHandle(p.hCompletionEventOrContext);
}

HRESULT WINAPI StreamFlush(void* t) {
    Stream* s = FromTitle(t);
    std::deque<Packet*> flushed;
    {
        std::lock_guard<std::recursive_mutex> lk(g_mu);
        flushed.swap(s->queue);
        if (s->voice) audio::Flush(s->voice);
    }
    for (Packet* pk : flushed) {
        CompletePacket(s, pk->p, kFlushed);
        delete pk;
    }
    return S_OK;
}

ULONG WINAPI StreamAddRef(void* t) { return os::AtomicIncrement(&FromTitle(t)->refs); }

// The title frees packet memory right after the final Release, so pending
// packets are completed (as flushed) before returning.
ULONG WINAPI StreamRelease(void* t) {
    Stream* s = FromTitle(t);
    LONG n = os::AtomicDecrement(&s->refs);
    if (n == 0) {
        StreamFlush(t);
        std::lock_guard<std::recursive_mutex> lk(g_mu);
        audio::DestroyVoice(s->voice);
        s->voice = nullptr;
    }
    return n;
}

HRESULT WINAPI StreamGetInfo(void* t, DWORD* info) {
    Stream* s = FromTitle(t);
    // XMEDIAINFO: dwFlags, dwInputSize, dwOutputSize, dwMaxLookahead
    info[0] = 0x2;  // XMO_STREAMF_FIXED_SAMPLE_SIZE
    info[1] = s->format.nBlockAlign ? s->format.nBlockAlign : 4;
    info[2] = info[3] = 0;
    return S_OK;
}

HRESULT WINAPI StreamGetStatus(void* t, DWORD* status) {
    Stream* s = FromTitle(t);
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    *status = s->queue.size() < s->max_packets ? 0x1 : 0;  // XMO_STATUSF_ACCEPT_INPUT_DATA
    return S_OK;
}

HRESULT WINAPI StreamProcess(void* t, const XMediaPacket* in, const XMediaPacket*) {
    Stream* s = FromTitle(t);
    if (!in) return E_INVALIDARG;
    std::unique_lock<std::recursive_mutex> lk(g_mu);
    if (in->pdwStatus) *in->pdwStatus = DWORD(kPending);
    if (in->pdwCompletedSize) *in->pdwCompletedSize = 0;
    auto* pk = new Packet{*in};
    pk->id = s->next_id++;
    s->queue.push_back(pk);
    audio::VoiceBuffer vb;
    if (IsAdpcm(s->format)) {
        pk->decoded = adpcm::Decode(in->pvBuffer.get(), in->dwMaxSize, s->host.nChannels);
        vb.data = pk->decoded.data();
        vb.frames = uint32_t(pk->decoded.size() / s->host.nChannels);
    } else {
        vb.data = in->pvBuffer.get();
        vb.frames = in->dwMaxSize / s->host.nBlockAlign;
    }
    vb.context = pk->id;
    if (!s->voice || !audio::Submit(s->voice, vb)) {
        s->queue.pop_back();
        lk.unlock();
        CompletePacket(s, pk->p, S_OK);  // nothing playable: complete at once
        delete pk;
    }
    return S_OK;
}

HRESULT WINAPI StreamDiscontinuity(void*) { return S_OK; }

uint32_t StreamVtable() {
    static uint32_t vt = [] {
        auto* v = static_cast<uint32_t*>(GuestAlloc(7 * 4));
#define KT_SLOT(i, f) v[i] = HleGuestCallable((void*)&f, &HleInvoker<&f>::Call, HleConv::Stdcall)
        KT_SLOT(0, StreamAddRef), KT_SLOT(1, StreamRelease), KT_SLOT(2, StreamGetInfo), KT_SLOT(3, StreamGetStatus);
        KT_SLOT(4, StreamProcess), KT_SLOT(5, StreamDiscontinuity), KT_SLOT(6, StreamFlush);
#undef KT_SLOT
        return H2G(v);
    }();
    return vt;
}

// Objects the title holds pointers to are constructed in guest memory.
template <typename T>
T* NewInGuest() {
    return new (GuestAlloc(sizeof(T), PAGE_READWRITE, 16)) T();
}

void* DsoundObject() {  // IDirectSound singleton (only its address matters)
    static void* p = GuestAlloc(0x100);
    return p;
}

float MixBinVolume(const MixBins* bins) {
    if (!bins || !bins->pairs) return 1;
    LONG best = -10000;
    for (DWORD i = 0; i < bins->dwMixBinCount; i++) best = std::max(best, bins->pairs.get()[i].lVolume);
    return Amplitude(best);
}

}  // namespace

// ---- IDirectSound ----------------------------------------------------------------------------

HRESULT WINAPI x_DirectSoundCreate(const GUID*, uint32_t* out, void*) {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    audio::Init();
    *out = H2G(DsoundObject());
    return S_OK;
}

ULONG WINAPI x_IDirectSound_AddRef(void*) { return 1; }
ULONG WINAPI x_IDirectSound_Release(void*) { return 1; }
void WINAPI x_DirectSoundDoWork() {}

HRESULT WINAPI x_IDirectSound_GetTime(void*, LONGLONG* t) {
    static const auto start = Clock::now();
    *t = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count() / 100;
    return S_OK;
}

// The effects image (DSP code for the APU) is accepted but not run.
HRESULT WINAPI x_IDirectSound_DownloadEffectsImage(void*, const void*, DWORD size, void*, uint32_t* desc) {
    Log("DownloadEffectsImage: %lu bytes ignored (no APU DSP emulation)", size);
    if (desc) {
        static void* empty_desc = GuestAlloc(64 * 4);  // DSEFFECTIMAGEDESC with zero effects
        *desc = H2G(empty_desc);
    }
    return S_OK;
}

HRESULT WINAPI x_IDirectSound_CreateSoundBuffer(void*, const XBufferDesc* d, uint32_t* out, void*) {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    auto* b = NewInGuest<Buffer>();
    if (d->lpwfxFormat) b->format = *d->lpwfxFormat;
    b->host = HostFormatOf(b->format);
    b->bytes = d->dwBufferBytes;
    if (b->bytes) b->data = static_cast<uint8_t*>(GuestAlloc(b->bytes));
    *out = H2G(b);
    return S_OK;
}

HRESULT WINAPI x_DirectSoundCreateBuffer(const XBufferDesc* d, uint32_t* out) {
    return x_IDirectSound_CreateSoundBuffer(nullptr, d, out, nullptr);
}

// ---- IDirectSoundBuffer --------------------------------------------------------------------------

ULONG WINAPI x_IDirectSoundBuffer_AddRef(Buffer* b) { return os::AtomicIncrement(&b->refs); }

ULONG WINAPI x_IDirectSoundBuffer_Release(Buffer* b) {
    LONG n = os::AtomicDecrement(&b->refs);
    if (n == 0) {
        std::lock_guard<std::recursive_mutex> lk(g_mu);
        audio::DestroyVoice(b->voice);
        b->voice = nullptr;
        b->playing = false;
    }
    return n;
}

HRESULT WINAPI x_IDirectSoundBuffer_SetBufferData(Buffer* b, void* data, DWORD bytes) {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    if (b->playing) b->Halt();
    b->data = static_cast<const uint8_t*>(data), b->bytes = bytes;
    b->decoded_from = nullptr;
    b->base_sample = 0;
    return S_OK;
}

HRESULT WINAPI x_IDirectSoundBuffer_Play(Buffer* b, DWORD, DWORD, DWORD flags) {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    bool looping = (flags & 1) != 0;  // DSBPLAY_LOOPING
    if (b->playing && b->looping == looping) return S_OK;
    uint32_t pos = b->playing ? b->Cursor() : b->base_sample;
    b->looping = looping;
    b->playing = true;
    b->StartAt(pos);
    return S_OK;
}

HRESULT WINAPI x_IDirectSoundBuffer_Stop(Buffer* b) {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    b->Halt();
    return S_OK;
}

HRESULT WINAPI x_IDirectSoundBuffer_StopEx(Buffer* b, LONGLONG, DWORD) { return x_IDirectSoundBuffer_Stop(b); }

HRESULT WINAPI x_IDirectSoundBuffer_SetCurrentPosition(Buffer* b, DWORD pos) {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    uint32_t s = b->ToSamples(pos);
    if (b->playing)
        b->StartAt(s);
    else
        b->base_sample = s;
    return S_OK;
}

HRESULT WINAPI x_IDirectSoundBuffer_GetCurrentPosition(Buffer* b, DWORD* play, DWORD* write) {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    DWORD p = b->ToBytes(b->Cursor());
    if (play) *play = p;
    if (write) *write = b->bytes ? (p + 1024) % b->bytes : 0;
    return S_OK;
}

HRESULT WINAPI x_IDirectSoundBuffer_GetStatus(Buffer* b, DWORD* status) {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    b->Cursor();  // updates `playing` for one-shot buffers that ran out
    if (b->playing && b->voice && !b->looping && audio::BuffersQueued(b->voice) == 0)
        b->playing = false, b->base_sample = 0;
    *status = (b->playing ? 1 : 0) | (b->playing && b->looping ? 4 : 0);
    return S_OK;
}

HRESULT WINAPI x_IDirectSoundBuffer_SetFrequency(Buffer* b, DWORD hz) {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    double base = std::max<DWORD>(b->host.nSamplesPerSec, 1);
    b->frequency_ratio = std::clamp(hz ? hz / base : 1.0, 1.0 / 1024, 4.0);
    if (b->voice) audio::SetFrequencyRatio(b->voice, b->frequency_ratio);
    return S_OK;
}

HRESULT WINAPI x_IDirectSoundBuffer_SetLoopRegion(Buffer* b, DWORD start, DWORD length) {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    b->loop_start = start, b->loop_length = length;
    return S_OK;
}

HRESULT WINAPI x_IDirectSoundBuffer_SetVolume(Buffer* b, LONG volume) {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    b->volume = Amplitude(volume);
    if (b->voice) audio::SetVolume(b->voice, b->volume);
    return S_OK;
}

HRESULT WINAPI x_IDirectSoundBuffer_SetMixBinVolumes_8(Buffer* b, const MixBins* bins) {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    b->volume = MixBinVolume(bins);
    if (b->voice) audio::SetVolume(b->voice, b->volume);
    return S_OK;
}

// ---- Streams ------------------------------------------------------------------------------------

HRESULT WINAPI x_DirectSoundCreateStream(const XStreamDesc* d, uint32_t* out) {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    audio::Init();
    auto* s = NewInGuest<Stream>();
    s->vtbl = StreamVtable();
    s->desc = *d;
    if (d->lpwfxFormat) s->format = *d->lpwfxFormat;
    s->host = HostFormatOf(s->format);
    s->max_packets = std::max<DWORD>(d->dwMaxAttachedPackets, 1);
    s->voice = NewVoice(s->host, OnPacketEnd, s);
    audio::Start(s->voice);
    Log("DirectSoundCreateStream: fmt %u, %u ch, %lu Hz, %lu packets, %s", s->format.wFormatTag, s->format.nChannels,
        s->format.nSamplesPerSec, s->max_packets, audio::SinkName());
    *out = H2G(&s->vtbl);
    return S_OK;
}

HRESULT WINAPI x_IDirectSoundStream_Pause(void* t, DWORD pause) {
    Stream* s = FromTitle(t);
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    s->paused = pause == 1;  // DSSTREAMPAUSE_PAUSE
    if (s->paused)
        audio::Stop(s->voice);
    else
        audio::Start(s->voice);
    return S_OK;
}

HRESULT WINAPI x_IDirectSoundStream_SetVolume(void* t, LONG volume) {
    Stream* s = FromTitle(t);
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    s->volume = Amplitude(volume);
    audio::SetVolume(s->voice, s->volume);
    return S_OK;
}

HRESULT WINAPI x_IDirectSoundStream_SetMixBinVolumes_8(void* t, const MixBins* bins) {
    Stream* s = FromTitle(t);
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    s->volume = MixBinVolume(bins);
    audio::SetVolume(s->voice, s->volume);
    return S_OK;
}

HLE_EXPORT("DSOUND", DirectSoundCreate);
HLE_EXPORT("DSOUND", IDirectSound_AddRef);
HLE_EXPORT("DSOUND", IDirectSound_Release);
HLE_EXPORT("DSOUND", DirectSoundDoWork);
HLE_EXPORT("DSOUND", IDirectSound_GetTime);
HLE_EXPORT("DSOUND", IDirectSound_DownloadEffectsImage);
HLE_EXPORT("DSOUND", IDirectSound_CreateSoundBuffer);
HLE_EXPORT("DSOUND", DirectSoundCreateBuffer);
HLE_EXPORT("DSOUND", IDirectSoundBuffer_AddRef);
HLE_EXPORT("DSOUND", IDirectSoundBuffer_Release);
HLE_EXPORT("DSOUND", IDirectSoundBuffer_SetBufferData);
HLE_EXPORT("DSOUND", IDirectSoundBuffer_Play);
HLE_EXPORT("DSOUND", IDirectSoundBuffer_Stop);
HLE_EXPORT("DSOUND", IDirectSoundBuffer_StopEx);
HLE_EXPORT("DSOUND", IDirectSoundBuffer_SetCurrentPosition);
HLE_EXPORT("DSOUND", IDirectSoundBuffer_GetCurrentPosition);
HLE_EXPORT("DSOUND", IDirectSoundBuffer_GetStatus);
HLE_EXPORT("DSOUND", IDirectSoundBuffer_SetFrequency);
HLE_EXPORT("DSOUND", IDirectSoundBuffer_SetLoopRegion);
HLE_EXPORT("DSOUND", IDirectSoundBuffer_SetVolume);
HLE_EXPORT("DSOUND", IDirectSoundBuffer_SetMixBinVolumes_8);
HLE_EXPORT("DSOUND", DirectSoundCreateStream);
HLE_EXPORT("DSOUND", IDirectSoundStream_Pause);
HLE_EXPORT("DSOUND", IDirectSoundStream_SetVolume);
HLE_EXPORT("DSOUND", IDirectSoundStream_SetMixBinVolumes_8);

// Effects/3D parameters with no equivalent without the APU's DSP.
HLE_IGNORE("DSOUND", IDirectSoundBuffer_SetFilter);
HLE_IGNORE("DSOUND", IDirectSoundBuffer_SetHeadroom);
HLE_IGNORE("DSOUND", IDirectSoundBuffer_SetLFO);
HLE_IGNORE("DSOUND", IDirectSoundBuffer_SetEG);
HLE_IGNORE("DSOUND", IDirectSoundBuffer_SetPitch);
HLE_IGNORE("DSOUND", IDirectSoundBuffer_SetMixBins);
HLE_IGNORE("DSOUND", IDirectSoundStream_SetHeadroom);
HLE_IGNORE("DSOUND", IDirectSoundStream_SetFrequency);
HLE_IGNORE("DSOUND", IDirectSound_SetMixBinHeadroom);
HLE_IGNORE("DSOUND", IDirectSound_SetI3DL2Listener);
HLE_IGNORE("DSOUND", IDirectSound_SetPosition);
HLE_IGNORE("DSOUND", IDirectSound_SetOrientation);
HLE_IGNORE("DSOUND", IDirectSound_CommitDeferredSettings);
HLE_IGNORE("DSOUND", IDirectSound_SetEffectData);
