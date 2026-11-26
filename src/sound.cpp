#include "sound.hpp"

#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <vector>

namespace sparring {

namespace {

constexpr int kRate = 22050;
bool g_on = true;

struct Note {
    float from_hz, to_hz;   // a glide when they differ
    float start, length;    // seconds
    float volume;           // 0..1
};

// A mono 16-bit WAV file in memory: soft sine notes that fade out, with a little warmth from the second harmonic.
std::vector<char> make_wav(std::initializer_list<Note> notes, float seconds) {
    const int count = static_cast<int>(seconds * kRate);
    std::vector<float> mix(static_cast<std::size_t>(count), 0.0f);
    for (const Note& n : notes) {
        const int first = static_cast<int>(n.start * kRate);
        const int last = std::min(count, static_cast<int>((n.start + n.length) * kRate));
        double phase = 0;
        for (int i = first; i < last; ++i) {
            const float t = static_cast<float>(i - first) / kRate;
            const float hz = n.from_hz + (n.to_hz - n.from_hz) * (t / n.length);
            phase += 2.0 * 3.14159265358979 * hz / kRate;
            const float attack = std::min(1.0f, t / 0.008f);
            const float decay = std::exp(-t * 4.5f / n.length);
            const float wave = static_cast<float>(std::sin(phase) * 0.82 + std::sin(2 * phase) * 0.18);
            mix[static_cast<std::size_t>(i)] += wave * attack * decay * n.volume;
        }
    }
    std::vector<char> wav(44 + static_cast<std::size_t>(count) * 2);
    auto put32 = [&](std::size_t at, std::uint32_t v) { std::memcpy(&wav[at], &v, 4); };
    auto put16 = [&](std::size_t at, std::uint16_t v) { std::memcpy(&wav[at], &v, 2); };
    std::memcpy(&wav[0], "RIFF", 4);
    put32(4, static_cast<std::uint32_t>(wav.size() - 8));
    std::memcpy(&wav[8], "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, 1);                 // PCM
    put16(22, 1);                 // mono
    put32(24, kRate);
    put32(28, kRate * 2);
    put16(32, 2);
    put16(34, 16);
    std::memcpy(&wav[36], "data", 4);
    put32(40, static_cast<std::uint32_t>(count * 2));
    for (int i = 0; i < count; ++i) {
        const float s = std::clamp(mix[static_cast<std::size_t>(i)], -1.0f, 1.0f);
        put16(44 + static_cast<std::size_t>(i) * 2, static_cast<std::uint16_t>(static_cast<std::int16_t>(s * 32000.0f)));
    }
    return wav;
}

const std::vector<char>& wav_for(Sound s) {
    static const std::vector<char> right = make_wav({{1046.5f, 1046.5f, 0.0f, 0.14f, 0.22f}, {1318.5f, 1318.5f, 0.09f, 0.42f, 0.22f},
                                                     {1568.0f, 1568.0f, 0.09f, 0.42f, 0.10f}},
                                                    0.55f);
    static const std::vector<char> wrong = make_wav({{330.0f, 247.0f, 0.0f, 0.26f, 0.24f}}, 0.3f);
    static const std::vector<char> cheer = make_wav({{1046.5f, 1046.5f, 0.0f, 0.18f, 0.18f}, {1318.5f, 1318.5f, 0.08f, 0.18f, 0.18f},
                                                     {1568.0f, 1568.0f, 0.16f, 0.18f, 0.18f}, {2093.0f, 2093.0f, 0.24f, 0.6f, 0.2f},
                                                     {1568.0f, 1568.0f, 0.24f, 0.6f, 0.08f}},
                                                    0.9f);
    static const std::vector<char> chest = make_wav({{1568.0f, 1568.0f, 0.0f, 0.2f, 0.14f}, {2093.0f, 2093.0f, 0.06f, 0.22f, 0.14f},
                                                     {2637.0f, 2637.0f, 0.12f, 0.3f, 0.14f}, {3136.0f, 3136.0f, 0.18f, 0.5f, 0.12f}},
                                                    0.75f);
    switch (s) {
    case Sound::Right: return right;
    case Sound::Wrong: return wrong;
    case Sound::Cheer: return cheer;
    case Sound::Chest: return chest;
    }
    return right;
}

}  // namespace

void set_sound_on(bool on) {
    g_on = on;
    if (!on) PlaySoundW(nullptr, nullptr, 0);
}

void play(Sound s) {
    if (!g_on) return;
    PlaySoundW(reinterpret_cast<LPCWSTR>(wav_for(s).data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

}  // namespace sparring
