// Xbox ADPCM (WAVE_FORMAT_XBOX_ADPCM, 0x69) decoding.
//
// A variant of IMA ADPCM: each block holds, per channel, a 4-byte header
// (initial sample, step index) followed by 32 bytes of 4-bit codes stored
// in 4-byte groups interleaved across channels. A block decodes to 65
// samples per channel (the header sample plus 64 codes).
#pragma once

#include <cstdint>
#include <vector>

namespace adpcm {

constexpr uint16_t kFormatTag = 0x69;
constexpr int kBlockBytesPerChannel = 36;
constexpr int kSamplesPerBlock = 65;

// Decodes whole blocks of `bytes` into interleaved 16-bit PCM.
std::vector<int16_t> Decode(const uint8_t* data, size_t bytes, int channels);

// Byte offset <-> sample position helpers for buffer cursors.
inline uint32_t BytesToSamples(uint32_t bytes, int channels) {
    return bytes / (kBlockBytesPerChannel * channels) * kSamplesPerBlock;
}
inline uint32_t SamplesToBytes(uint32_t samples, int channels) {
    return samples / kSamplesPerBlock * (kBlockBytesPerChannel * channels);
}

}  // namespace adpcm
