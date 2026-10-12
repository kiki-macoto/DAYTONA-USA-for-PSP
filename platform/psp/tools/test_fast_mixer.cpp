// game/fast_mixer.h against the desktop's snd::NativeSoundEngine on the PC,
// both at 48 kHz on the same commands: sequences started on MIDI channel 14
// (0xAE bank index, as the game sends them), the music bank (16) and the
// effects bank (33). Prints the signal-to-error ratio of each and the time
// each engine took. Your own ROM images (build-daytona/rom_cache/daytona).
//
// Built from the repository's root with
//   c++ -std=c++20 -O2 -I src -I platform/psp/game platform/psp/tools/test_fast_mixer.cpp
//       src/runtime/native_sample_mixer.cpp src/runtime/native_sound_sequencer.cpp
//       src/runtime/native_sound_engine.cpp -o test_fast_mixer
// (one command), then: ./test_fast_mixer build-daytona/rom_cache/daytona
#include "fast_mixer.h"
#include "runtime/native_sound_engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> load(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", path.c_str());
        std::exit(2);
    }
    return {std::istreambuf_iterator<char>(f), {}};
}

struct Result { double snr_db, reference_rms, ms_reference, ms_fast; size_t streamed_differences; };

// `seconds` of sequence (bank, index), 512 frames a call, as the game would.
// The PCM ROMs read through a reader (the PSP without its extra memory),
// with a small decode cache, must give the in-RAM engine's samples exactly.
Result compare(const std::string &dir, uint8_t bank, uint8_t index, int seconds) {
    const auto program = load(dir + "/sound_program.bin");
    const auto pcm1 = load(dir + "/pcm1.bin"), pcm2 = load(dir + "/pcm2.bin");
    snd::NativeSoundEngine reference(program, pcm1, pcm2);
    psp::FastSoundEngine fast(program, pcm1, pcm2, 48000);
    const auto header = [](const std::vector<uint8_t> &pcm) {
        return std::vector<uint8_t>(pcm.begin(), pcm.begin() + psp::FastSoundEngine::kHeaderBytes);
    };
    psp::FastSoundEngine streamed(
        program, header(pcm1), pcm1.size(), header(pcm2), pcm2.size(),
        [&](unsigned rom, uint32_t offset, uint8_t *to, size_t n) {
            const auto &pcm = rom ? pcm2 : pcm1;
            if (offset > pcm.size() || n > pcm.size() - offset) return false;
            std::copy_n(pcm.begin() + offset, n, to);
            return true;
        },
        48000, 256 * 1024);
    const uint8_t start[] = {0xae, bank, index};
    reference.send(start, sizeof start);
    fast.send(start, sizeof start);
    streamed.send(start, sizeof start);
    std::vector<float> a(1024);
    std::vector<int16_t> b(1024), c(1024);
    size_t differences = 0;
    double signal = 0, error = 0, ms_a = 0, ms_b = 0;
    const size_t total = size_t(seconds) * 48000;
    for (size_t done = 0; done < total; done += 512) {
        auto t0 = std::chrono::steady_clock::now();
        reference.render(a.data(), 512);
        auto t1 = std::chrono::steady_clock::now();
        fast.render(b.data(), 512);
        auto t2 = std::chrono::steady_clock::now();
        ms_a += std::chrono::duration<double, std::milli>(t1 - t0).count();
        ms_b += std::chrono::duration<double, std::milli>(t2 - t1).count();
        streamed.render(c.data(), 512);
        for (size_t i = 0; i < 1024; ++i) differences += b[i] != c[i];
        for (size_t i = 0; i < 1024; ++i) {
            const double r = a[i], f = b[i] / 32768.0;
            signal += r * r;
            error += (r - f) * (r - f);
        }
    }
    const double samples = double(total) * 2;
    return {error > 0 ? 10 * std::log10(signal / error) : 999.0, std::sqrt(signal / samples), ms_a, ms_b,
            differences};
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s ROM_CACHE_DIR\n", argv[0]);
        return 2;
    }
    int failures = 0, played = 0;
    double ms_a = 0, ms_b = 0;
    for (const auto &[bank, index, seconds] : {std::tuple{16, 0, 20}, {16, 1, 20}, {16, 2, 20}, {16, 3, 20},
                                               {33, 0, 3}, {33, 5, 3}, {33, 10, 3}, {33, 20, 3}}) {
        const Result r = compare(argv[1], uint8_t(bank), uint8_t(index), seconds);
        ms_a += r.ms_reference;
        ms_b += r.ms_fast;
        if (r.reference_rms < 1e-4) {
            std::printf("bank %2d index %2d: silent in the reference, skipped\n", bank, index);
            continue;
        }
        ++played;
        const bool ok = r.snr_db >= 30.0 && !r.streamed_differences;
        failures += !ok;
        std::printf("bank %2d index %2d: rms %.4f, signal/error %.1f dB, read through a reader: %zu samples differ %s\n",
                    bank, index, r.reference_rms, r.snr_db, r.streamed_differences,
                    ok ? "ok" : "FAIL (under 30 dB, or the reader's differ)");
    }
    std::printf("time: reference %.0f ms, fast %.0f ms (%.1fx)\n", ms_a, ms_b, ms_a / ms_b);
    if (!played) {
        std::printf("test_fast_mixer: nothing played\n");
        return EXIT_FAILURE;
    }
    std::printf(failures ? "test_fast_mixer: %d failed\n" : "test_fast_mixer: all passed\n", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
