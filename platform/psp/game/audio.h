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
// With the Media Engine (media_engine.h; MeLink given): the game thread runs
// the sequencer and writes the frame's mixing as jobs (fast_mixer.h,
// offload), the engine mixes them into the ring (me_worker) while the game
// runs on; the output thread plays them as soon as the engine is done
// (posted_end_), and the next frame waits for it (it is long done) before
// it renders more. 44.1 kHz
// through the plain output channel then (the converter may itself use the
// Media Engine). If the engine does not finish a batch within 200 ms, the
// game thread mixes from then on.
//
// The PCM ROMs (8 MB) are held in RAM when the extra memory is there, else
// read from the Memory Stick a sample at a time as it is first played (the
// game thread waits for the read; the ring's 96 ms keeps the output going).
#pragma once

#include "fast_mixer.h"
#include "media_engine.h"

#include <malloc.h>
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

// The game thread and the Media Engine's meeting place, read and written
// uncached by both (64 bytes, its own line).
struct alignas(64) MeLink {
    volatile uint32_t alive, seq, done, unused;
    const uint8_t *volatile jobs;
    volatile uint32_t bytes;
    FastSoundEngine::MixState *volatile state;
    uint32_t pad[9];
};
inline MeLink *g_me_link = nullptr; // (the uncached address)

// On the Media Engine: each batch the game thread posts (seq), mixed, its
// cache written back, then done.
inline void me_worker() {
    MeLink *const link = g_me_link;
    uint32_t seen = link->seq;
    link->alive = 1;
    for (;;) {
        while (link->seq == seen) me::pause();
        seen = link->seq;
        me::dcache_writeback_invalidate_all(); // (nothing stale: the jobs, the samples)
        FastSoundEngine::run_jobs(link->jobs, link->bytes, *link->state);
        me::dcache_writeback_invalidate_all(); // the ring in memory
        __asm__ __volatile__("sync" ::: "memory");
        link->done = seen;
    }
}

class Audio {
public:
    Audio(std::vector<uint8_t> program, std::vector<uint8_t> pcm1, std::vector<uint8_t> pcm2, MeLink *me = nullptr)
        : rate_(me ? kRateMe : kRate), engine_(std::move(program), std::move(pcm1), std::move(pcm2), rate_), me_(me) {
        start();
    }
    // The PCM ROMs read through `reader` (FastSoundEngine's second form).
    Audio(std::vector<uint8_t> program, std::vector<uint8_t> header1, size_t bytes1, std::vector<uint8_t> header2,
          size_t bytes2, FastSoundEngine::PcmReader reader, size_t cache_bytes, MeLink *me = nullptr)
        : rate_(me ? kRateMe : kRate), engine_(std::move(program), std::move(header1), bytes1, std::move(header2),
                                               bytes2, std::move(reader), rate_, cache_bytes),
          me_(me) {
        start();
    }
    ~Audio() {
        stop_ = true;
        sceKernelWaitThreadEnd(thread_, nullptr);
        sceKernelDeleteThread(thread_);
        if (channel_ >= 0) sceAudioChRelease(channel_);
        else sceAudioSRCChRelease();
        instance_ = nullptr;
        std::free(jobs_);
        std::free(state_);
    }
    Audio(const Audio &) = delete;
    Audio &operator=(const Audio &) = delete;

private:
    void start() {
        if (me_) {
            jobs_ = static_cast<uint8_t *>(memalign(64, kJobBytes));
            state_ = static_cast<FastSoundEngine::MixState *>(memalign(64, sizeof(FastSoundEngine::MixState)));
            if (!jobs_ || !state_) throw std::runtime_error("no RAM for the Media Engine's jobs");
            engine_.init_mix_state(*state_);
            engine_.writeback = [](const void *data, size_t bytes) {
                sceKernelDcacheWritebackRange(data, unsigned(bytes));
            };
            engine_.offload(jobs_, kJobBytes);
            sceKernelDcacheWritebackRange(state_, sizeof *state_);
            sceKernelDcacheWritebackInvalidateRange(ring_, sizeof ring_); // (only the engine writes it now)
            me_->state = state_;
            channel_ = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, kBlock, PSP_AUDIO_FORMAT_STEREO);
            if (channel_ < 0) throw std::runtime_error("no audio output channel");
        } else if (sceAudioSRCChReserve(kBlock, kRate, 2) < 0) {
            throw std::runtime_error("no audio output channel");
        }
        instance_ = this;
        thread_ = sceKernelCreateThread("audio", output_thread, kPriority, 16 * 1024, PSP_THREAD_ATTR_USER, nullptr);
        if (thread_ < 0) {
            if (channel_ >= 0) sceAudioChRelease(channel_);
            else sceAudioSRCChRelease();
            throw std::runtime_error("cannot create the audio thread");
        }
        sceKernelStartThread(thread_, 0, nullptr);
    }

public:
    // Once a game frame, with the bytes the game sent the sound board: then
    // render what keeps the ring at its target.
    void frame(const std::vector<uint8_t> &bytes) {
        if (me_ && pending_) collect();
        if (!bytes.empty()) engine_.send(bytes.data(), bytes.size());
        barrier();
        const uint32_t fill = write_ - read_;
        if (fill >= target()) return;
        size_t frames = std::min<size_t>(target() - fill, kMaxRender);
        if (me_) engine_.begin_batch();
        while (frames) {
            // Straight into the ring, up to its wrap.
            const uint32_t at = (write_ + pending_) & (kRing - 1);
            const size_t n = std::min<size_t>(frames, kRing - at);
            engine_.render(&ring_[at * 2], n);
            barrier();
            if (me_) pending_ += uint32_t(n);
            else write_ += uint32_t(n);
            frames -= n;
        }
        if (me_ && pending_) post();
    }
    void set_volumes(float music, float effects) { engine_.set_volumes(music, effects); }
    unsigned voices() const { return engine_.active_voices(); }
    size_t cached_bytes() const { return engine_.cached_bytes(); }
    void set_profile_clock(uint64_t (*clock)()) { engine_.clock = clock; }
    uint64_t take_mix_us() { return std::exchange(engine_.mix_us, 0); } // (since the last take)
    uint64_t underruns = 0; // output blocks that found the ring short (filled with silence)
    // The Media Engine: in use; the game thread's microseconds waiting for
    // it (since the last take); whether it was given up (a batch not done
    // in 200 ms); jobs that did not fit.
    bool media_engine() const { return me_ != nullptr; }
    uint64_t take_me_wait_us() { return std::exchange(me_wait_us_, 0); }
    bool me_given_up = false;
    uint64_t job_overflows() const { return engine_.job_overflows; }
    unsigned rate() const { return rate_; }

private:
    static constexpr unsigned kRate = 32000;   // the mix rate (the output channel converts it)
    static constexpr unsigned kRateMe = 44100; // with the Media Engine: the plain channel's
    static constexpr int kBlock = 1024;        // frames per hardware output call (32 ms; 23 ms at 44.1 kHz)
    static constexpr uint32_t kRing = 8192;    // frames (a power of two)
    static constexpr size_t kMaxRender = kRing - 2 * kBlock;
    static constexpr int kPriority = 0x12; // above the game thread's: the output must not starve
    static constexpr size_t kJobBytes = 256 * 1024;
    // What each game frame tops the ring up to: 3 output blocks (96 ms at
    // 32 kHz); with the Media Engine 5 (116 ms at 44.1 kHz: its batch is
    // played once it is mixed, a few ms in, and frames ran to 40 ms).
    uint32_t target() const { return uint32_t(me_ ? 5 : 3) * kBlock; }

    static void barrier() { __asm__ __volatile__("" ::: "memory"); }

    // The engine's last batch: waited for (it is normally long done), then
    // its frames written. Given up after 200 ms: the game thread mixes.
    void collect() {
        const uint32_t t0 = sceKernelGetSystemTimeLow();
        while (me_->done != me_->seq) {
            if (sceKernelGetSystemTimeLow() - t0 > 200000) {
                engine_.offload(nullptr, 0);
                me_given_up = true;
                break;
            }
            sceKernelDelayThread(50);
        }
        me_wait_us_ += sceKernelGetSystemTimeLow() - t0;
        barrier();
        write_ += pending_;
        pending_ = 0;
        if (me_given_up) {
            me_ = nullptr; // (the output thread reads the ring cached again, as this thread writes it)
            sceKernelDcacheInvalidateRange(ring_, sizeof ring_);
        }
    }
    void post() {
        sceKernelDcacheWritebackRange(jobs_, unsigned(engine_.job_bytes()));
        me_->jobs = jobs_;
        me_->bytes = uint32_t(engine_.job_bytes());
        posted_end_ = write_ + pending_; // (before seq: the output thread reads it once done == seq)
        __asm__ __volatile__("sync" ::: "memory");
        me_->seq = me_->seq + 1;
    }

    static int output_thread(SceSize, void *) {
        pspSdkDisableFPUExceptions(); // (as the game thread: main.cpp's main)
        Audio &self = *instance_;
        while (!self.stop_) {
            barrier();
            // With the Media Engine, its batch as soon as it is mixed.
            MeLink *const me = self.me_;
            const uint32_t written = me && me->done == me->seq ? self.posted_end_ : self.write_;
            const uint32_t fill = written - self.read_;
            // The Media Engine's ring read uncached (its writes are in memory, not in this cache).
            const int16_t *ring = me ? reinterpret_cast<const int16_t *>(
                                                           reinterpret_cast<uintptr_t>(self.ring_) | 0x40000000u)
                                                     : self.ring_;
            for (uint32_t i = 0; i < uint32_t(kBlock); ++i) {
                int16_t *out = &self.block_[i * 2];
                if (i < fill) {
                    const int16_t *in = &ring[((self.read_ + i) & (kRing - 1)) * 2];
                    out[0] = in[0];
                    out[1] = in[1];
                } else {
                    out[0] = out[1] = 0;
                }
            }
            if (fill < uint32_t(kBlock)) ++self.underruns;
            barrier();
            self.read_ += std::min<uint32_t>(fill, kBlock);
            if (self.channel_ >= 0)
                sceAudioOutputPannedBlocking(self.channel_, PSP_AUDIO_VOLUME_MAX, PSP_AUDIO_VOLUME_MAX, self.block_);
            else
                sceAudioSRCOutputBlocking(PSP_AUDIO_VOLUME_MAX, self.block_);
        }
        return 0;
    }

    unsigned rate_;
    FastSoundEngine engine_;
    MeLink *volatile me_; // (read by the output thread too)
    alignas(64) int16_t ring_[kRing * 2] = {};
    alignas(64) int16_t block_[kBlock * 2] = {};
    volatile uint32_t write_ = 0, read_ = 0; // frames, ever increasing (wrapping); write_ by the game, read_ by the output
    uint32_t pending_ = 0;                   // frames in the Media Engine's batch, not yet in write_
    volatile uint32_t posted_end_ = 0;       // write_ + pending_ as the batch was posted
    volatile bool stop_ = false;
    SceUID thread_ = -1;
    int channel_ = -1; // the plain output channel (with the Media Engine), else the converter's
    uint8_t *jobs_ = nullptr;
    FastSoundEngine::MixState *state_ = nullptr;
    uint64_t me_wait_us_ = 0;
    static inline Audio *instance_ = nullptr;
};

} // namespace psp
