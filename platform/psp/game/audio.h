// The PSP's sound: the desktop's native sequencer (src/runtime/
// native_sound_sequencer) fed the bytes the game sends its sound board, its
// voices mixed by fast_mixer.h (integer arithmetic, samples decoded once) at
// 32 kHz, through the PSP's sample-rate-converting output channel.
//
// The engine is run by the game thread, once a frame (it is not thread-safe,
// and the game thread is the only one sending it bytes); what it renders goes
// into a ring the output thread empties at the hardware's pace. Each frame the
// game thread renders what keeps the ring at kTarget frames, so the music and
// effects play in real time whatever the game's frame rate (as the
// Dreamcast's sound, which advances by the elapsed time); an underrun plays
// silence. The PSP has one core for both threads: no atomics (Allegrex has no
// LL/SC), the indices are each written by one thread, with compiler barriers.
//
// The PCM ROMs (8 MB) are held in RAM when the extra memory is there, else
// read from the Memory Stick a sample at a time as it is first played (the
// game thread waits for the read; the ring's 96 ms keeps the output going).
#pragma once

#include "fast_mixer.h"

#include <pspaudio.h>
#include <pspkernel.h>
#include <pspsdk.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace psp {

class Audio {
public:
    Audio(std::vector<uint8_t> program, std::vector<uint8_t> pcm1, std::vector<uint8_t> pcm2)
        : engine_(std::move(program), std::move(pcm1), std::move(pcm2), kRate) {
        start();
    }
    // The PCM ROMs read through `reader` (FastSoundEngine's second form).
    Audio(std::vector<uint8_t> program, std::vector<uint8_t> header1, size_t bytes1, std::vector<uint8_t> header2,
          size_t bytes2, FastSoundEngine::PcmReader reader, size_t cache_bytes)
        : engine_(std::move(program), std::move(header1), bytes1, std::move(header2), bytes2, std::move(reader), kRate,
                  cache_bytes) {
        start();
    }
    ~Audio() {
        stop_ = true;
        sceKernelWaitThreadEnd(thread_, nullptr);
        sceKernelDeleteThread(thread_);
        sceAudioSRCChRelease();
        instance_ = nullptr;
    }
    Audio(const Audio &) = delete;
    Audio &operator=(const Audio &) = delete;

private:
    void start() {
        if (sceAudioSRCChReserve(kBlock, kRate, 2) < 0) throw std::runtime_error("no audio output channel");
        instance_ = this;
        thread_ = sceKernelCreateThread("audio", output_thread, kPriority, 16 * 1024, PSP_THREAD_ATTR_USER, nullptr);
        if (thread_ < 0) {
            sceAudioSRCChRelease();
            throw std::runtime_error("cannot create the audio thread");
        }
        sceKernelStartThread(thread_, 0, nullptr);
    }

public:
    // Once a game frame, with the bytes the game sent the sound board: then
    // render what keeps the ring at its target.
    void frame(const std::vector<uint8_t> &bytes) {
        if (!bytes.empty()) engine_.send(bytes.data(), bytes.size());
        barrier();
        const uint32_t fill = write_ - read_;
        if (fill >= kTarget) return;
        size_t frames = std::min<size_t>(kTarget - fill, kMaxRender);
        while (frames) {
            // Straight into the ring, up to its wrap.
            const uint32_t at = write_ & (kRing - 1);
            const size_t n = std::min<size_t>(frames, kRing - at);
            engine_.render(&ring_[at * 2], n);
            barrier();
            write_ += uint32_t(n);
            frames -= n;
        }
    }
    void set_volumes(float music, float effects) { engine_.set_volumes(music, effects); }
    unsigned voices() const { return engine_.active_voices(); }
    size_t cached_bytes() const { return engine_.cached_bytes(); }
    void set_profile_clock(uint64_t (*clock)()) { engine_.clock = clock; }
    uint64_t take_mix_us() { return std::exchange(engine_.mix_us, 0); } // (since the last take)
    uint64_t underruns = 0; // output blocks that found the ring short (filled with silence)

private:
    static constexpr int kRate = 32000;     // the mix rate (the output channel converts it)
    static constexpr int kBlock = 1024;     // frames per hardware output call (32 ms)
    static constexpr uint32_t kRing = 8192; // frames (a power of two)
    static constexpr uint32_t kTarget = 3 * kBlock; // what each game frame tops the ring up to (96 ms)
    static constexpr size_t kMaxRender = kRing - kBlock;
    static constexpr int kPriority = 0x12; // above the game thread's: the output must not starve

    static void barrier() { __asm__ __volatile__("" ::: "memory"); }

    static int output_thread(SceSize, void *) {
        pspSdkDisableFPUExceptions(); // (as the game thread: main.cpp's main)
        Audio &self = *instance_;
        while (!self.stop_) {
            barrier();
            const uint32_t fill = self.write_ - self.read_;
            for (uint32_t i = 0; i < uint32_t(kBlock); ++i) {
                int16_t *out = &self.block_[i * 2];
                if (i < fill) {
                    const int16_t *in = &self.ring_[((self.read_ + i) & (kRing - 1)) * 2];
                    out[0] = in[0];
                    out[1] = in[1];
                } else {
                    out[0] = out[1] = 0;
                }
            }
            if (fill < uint32_t(kBlock)) ++self.underruns;
            barrier();
            self.read_ += std::min<uint32_t>(fill, kBlock);
            sceAudioSRCOutputBlocking(PSP_AUDIO_VOLUME_MAX, self.block_);
        }
        return 0;
    }

    FastSoundEngine engine_;
    alignas(64) int16_t ring_[kRing * 2] = {};
    alignas(64) int16_t block_[kBlock * 2] = {};
    volatile uint32_t write_ = 0, read_ = 0; // frames, ever increasing (wrapping); write_ by the game, read_ by the output
    volatile bool stop_ = false;
    SceUID thread_ = -1;
    static inline Audio *instance_ = nullptr;
};

} // namespace psp
