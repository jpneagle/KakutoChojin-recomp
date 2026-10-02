// Software voice mixer with a host audio sink.
//
// Voices play queued PCM buffers (8-bit unsigned or 16-bit signed, any
// channel count and rate) with looping, volume and pitch, and report each
// finished buffer through a callback. The mix goes to the SDL audio device,
// or, without one (or with KT_SILENT set), to a null sink that consumes it in
// real time, so buffer cursors and completions keep the same timing.
#pragma once

#include <cstddef>
#include <cstdint>

namespace audio {

struct Voice;

// Called on the mixer thread, with no mixer lock held, after a buffer ends.
using BufferEndFn = void (*)(void* owner, uint64_t context);

struct VoiceBuffer {
    const void* data = nullptr;
    uint32_t frames = 0;
    uint32_t play_begin = 0;                   // first frame played
    bool loop = false;                         // loop forever over [loop_begin, loop_begin + loop_length)
    uint32_t loop_begin = 0, loop_length = 0;  // loop_length 0: up to the end
    uint64_t context = 0;                      // passed to BufferEndFn
};

void Init();             // idempotent
const char* SinkName();  // "SDL (<driver>)" or "silent"

Voice* CreateVoice(int channels, int bits, int rate, BufferEndFn on_end, void* owner);
void DestroyVoice(Voice* v);           // no callbacks for its buffers afterwards
bool Submit(Voice* v, const VoiceBuffer& b);
void Flush(Voice* v);                  // drops queued buffers without callbacks
void Start(Voice* v);
void Stop(Voice* v);                   // pauses; the queue is kept
void SetVolume(Voice* v, float amplitude);
void SetFrequencyRatio(Voice* v, double ratio);
uint64_t FramesPlayed(Voice* v);       // source frames consumed since creation
size_t BuffersQueued(Voice* v);

}  // namespace audio
