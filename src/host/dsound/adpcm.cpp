#include "adpcm.h"

#include <algorithm>

namespace adpcm {

namespace {

const int kStepTable[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
    544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
    9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
const int kIndexTable[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

struct Channel {
    int predictor, index;
    int16_t Step(uint8_t code) {
        int step = kStepTable[index];
        int diff = step >> 3;
        if (code & 1) diff += step >> 2;
        if (code & 2) diff += step >> 1;
        if (code & 4) diff += step;
        if (code & 8) diff = -diff;
        predictor = std::clamp(predictor + diff, -32768, 32767);
        index = std::clamp(index + kIndexTable[code], 0, 88);
        return int16_t(predictor);
    }
};

}  // namespace

std::vector<int16_t> Decode(const uint8_t* data, size_t bytes, int channels) {
    std::vector<int16_t> out;
    if (channels < 1 || channels > 8) return out;
    const size_t block = size_t(kBlockBytesPerChannel) * channels;
    out.reserve(bytes / block * kSamplesPerBlock * channels);
    std::vector<Channel> ch(channels);
    std::vector<int16_t> frame(size_t(kSamplesPerBlock) * channels);
    for (size_t off = 0; off + block <= bytes; off += block) {
        const uint8_t* b = data + off;
        for (int c = 0; c < channels; c++) {
            const uint8_t* h = b + 4 * c;
            ch[c].predictor = int16_t(h[0] | h[1] << 8);
            ch[c].index = std::min<int>(h[2], 88);
            frame[c] = int16_t(ch[c].predictor);
        }
        // 8 groups of 4 bytes (8 codes) per channel, interleaved by channel.
        const uint8_t* codes = b + 4 * channels;
        for (int g = 0; g < 8; g++)
            for (int c = 0; c < channels; c++) {
                const uint8_t* p = codes + (g * channels + c) * 4;
                for (int i = 0; i < 8; i++) {
                    uint8_t code = (p[i / 2] >> ((i & 1) * 4)) & 0xF;
                    frame[size_t(1 + g * 8 + i) * channels + c] = ch[c].Step(code);
                }
            }
        out.insert(out.end(), frame.begin(), frame.end());
    }
    return out;
}

}  // namespace adpcm
