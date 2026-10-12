// The PSP's sound engine: the desktop's native sequencer (src/runtime/
// native_sound_sequencer, unchanged) with a mixer of its own in integer
// arithmetic, in place of snd::NativeSoundEngine's NativeSampleMixer, which
// on the PSP cost 10-12 ms of a race frame (in PPSSPP): a float divide in the
// envelope, 64-bit phases, two format-branching sample reads and float
// conversions per voice and output sample.
//
// The same voices, as NativeSampleMixer plays them (the engine's defaults:
// the header's loop, a 2 ms linear attack, sustain 1, a 20 ms linear
// release, equal-power pan, the master gain and the peak limiter), with:
// - samples decoded once to 16 bits when a note first needs them (through
//   NativeSampleBank::value, so the formats and bank windows are its own),
//   with one guard frame after the end (the loop start's frame, or the last)
//   so the interpolation reads the next frame without a branch; kept in a
//   cache of kCacheBytes, the least recently used not playing dropped;
// - phases as NativeSampleMixer's 32.32, but in two 32-bit words added with
//   a carry (no 64-bit arithmetic on the 32-bit Allegrex; a first 17.15
//   phase drifted by up to 0.008% in pitch, inaudible, but it moved whole
//   sequences out of phase against the reference over 20 s);
// - the envelope and the volumes stepped every kBlock output frames
//   (0.5 ms at 32 kHz) instead of every frame;
// - any output rate (the PSP plays 32 kHz: two thirds of 48 kHz's work);
// - the PCM ROMs either in RAM (8 MB) or read a sample at a time when it is
//   decoded (a PcmReader: the Memory Stick when the extra memory is
//   missing), to the same 16-bit values.
// Not bit-exact with NativeSampleMixer (it is not chip-exact either);
// tools/test_fast_mixer.cpp compares the two at 48 kHz on the PC.
//
// No PSP SDK here: the PC test builds it.
#pragma once

#include "runtime/native_sample_mixer.h"
#include "runtime/native_sound_sequencer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace psp {

class FastSoundEngine {
public:
    static constexpr unsigned kVoices = snd::NativeSampleMixer::kVoices;
    static constexpr size_t kCacheBytes = 3u << 20;
    static constexpr size_t kHeaderBytes = snd::NativeSampleBank::kSamples * 12; // a PCM ROM's sample table
    // Reads n bytes at a PCM ROM's (0 or 1) offset; false if it cannot.
    using PcmReader = std::function<bool(unsigned rom, uint32_t offset, uint8_t *to, size_t n)>;

    // The PCM ROMs in RAM.
    FastSoundEngine(std::vector<uint8_t> program, std::vector<uint8_t> pcm1, std::vector<uint8_t> pcm2,
                    uint32_t rate, size_t cache_bytes = kCacheBytes)
        : program_(std::move(program)), pcm_{std::move(pcm1), std::move(pcm2)},
          pcm_bytes_{pcm_[0].size(), pcm_[1].size()}, cache_limit_(cache_bytes), rate_(rate),
          sequencer_(program_, rate) {
        init();
    }
    // The PCM ROMs read through `reader`: only their sample tables (the first
    // kHeaderBytes) in RAM, with the ROMs' sizes.
    FastSoundEngine(std::vector<uint8_t> program, std::vector<uint8_t> header1, size_t bytes1,
                    std::vector<uint8_t> header2, size_t bytes2, PcmReader reader, uint32_t rate,
                    size_t cache_bytes)
        : program_(std::move(program)), pcm_{std::move(header1), std::move(header2)}, pcm_bytes_{bytes1, bytes2},
          reader_(std::move(reader)), cache_limit_(cache_bytes), rate_(rate), sequencer_(program_, rate) {
        for (unsigned rom = 0; rom < 2; ++rom)
            if (pcm_[rom].size() < kHeaderBytes) throw std::runtime_error("native audio: missing PCM sample table");
        init();
    }
    FastSoundEngine(const FastSoundEngine &) = delete;
    FastSoundEngine &operator=(const FastSoundEngine &) = delete;

private:
    void init() {
        // (Read through a reader, the bank is given the table alone with the
        // ROM's size: load reads only the table and checks the samples'
        // ranges against the size; value() is then never called.)
        for (unsigned rom = 0; rom < 2; ++rom)
            for (unsigned bank = 0; bank < 4; ++bank)
                if (!banks_[rom][bank].load(pcm_[rom].data(), pcm_bytes_[rom], bank))
                    throw std::runtime_error("native audio: missing PCM sample table");
        sequencer_.set_sink(event, this);
        attack_ = frames(0.002f);
        release_ = frames(0.020f);
        recovery_ = float(1.0 - std::exp(-1.0 / (double(rate_) * 0.050)));
    }

public:
    void send(const uint8_t *bytes, size_t count) {
        if (count) sequencer_.send(bytes, count);
    }
    // Interleaved stereo, 16 bits.
    void render(int16_t *stereo, size_t count) {
        if (!count) return;
        output_ = stereo;
        written_ = 0;
        end_ = cursor_ + count;
        sequencer_.advance(count);
        render_to(end_);
        output_ = nullptr;
    }
    void set_volumes(float music, float effects) {
        volumes_[0] = std::clamp(music, 0.0f, 1.0f);
        volumes_[1] = std::clamp(effects, 0.0f, 1.0f);
        for (Voice &v : voices_) gains(v);
    }
    unsigned active_voices() const {
        unsigned n = 0;
        for (const Voice &v : voices_) n += v.active;
        return n;
    }
    size_t cached_bytes() const { return cache_bytes_; }
    uint64_t decodes = 0, rejected = 0;
    // The mixing's own time (the rest of render is the sequencer), when a
    // clock is set: microseconds.
    uint64_t (*clock)() = nullptr;
    uint64_t mix_us = 0;

private:
    static constexpr unsigned kBlock = 16;
    static constexpr int32_t kOne = 1 << 15; // levels and samples: Q15
    static constexpr float kMasterGain = snd::NativeSampleMixer::kDefaultMasterGain;
    static constexpr float kCeiling = snd::NativeSampleMixer::kPeakCeiling;

    struct Decoded {
        std::vector<int16_t> data; // frames + 1 (the guard)
        uint64_t used = 0;
        unsigned playing = 0;
    };
    enum class Stage : uint8_t { Attack, Sustain, Release };
    struct Voice {
        bool active = false, effect = false;
        Stage stage = Stage::Sustain;
        Decoded *sample = nullptr;
        const int16_t *data = nullptr;
        uint32_t index = 0, frac = 0;          // the phase: frame, and 32 bits of fraction
        uint32_t step_index = 0, step_frac = 0; // the step, likewise
        uint32_t end = 0, loop = 0;            // frames
        bool looping = true;
        float gain = 0, pan = 0;
        int32_t gain_l = 0, gain_r = 0;  // Q12: gain * pan * volume * master
        int32_t level = 0, level_step = 0; // Q15 * 2^8 (sub-step precision)
        uint32_t stage_left = 0;           // output frames left in the attack or release
    };

    uint32_t frames(float seconds) const { return uint32_t(double(seconds) * rate_ + 0.5); }

    static void event(void *self, const snd::NativeSoundSequencer::VoiceEvent &e) {
        static_cast<FastSoundEngine *>(self)->apply(e);
    }
    void render_to(uint64_t frame) {
        if (frame < cursor_ || frame > end_) throw std::runtime_error("native audio: non-monotonic event timestamp");
        const size_t n = size_t(frame - cursor_);
        if (n) {
            const uint64_t t0 = clock ? clock() : 0;
            mix(output_ + written_ * 2, n);
            if (clock) mix_us += clock() - t0;
            written_ += n;
            cursor_ = frame;
        }
    }

    void apply(const snd::NativeSoundSequencer::VoiceEvent &e) {
        render_to(e.frame);
        if (e.voice_id >= kVoices) { ++rejected; return; }
        Voice &v = voices_[e.voice_id];
        using Kind = snd::NativeSoundSequencer::EventKind;
        switch (e.kind) {
        case Kind::NoteOn: note_on(v, e); break;
        case Kind::NoteOff:
            if (!v.active || v.stage == Stage::Release) break;
            if (!release_) { stop(v); break; }
            begin(v, Stage::Release, release_, 0);
            break;
        case Kind::Stop: stop(v); break;
        case Kind::Update:
            if (!v.active) break; // (a released one-shot may have ended)
            if (!set_step(v, e.source_rate_hz) || !(e.gain >= 0 && e.gain <= 4) || !(e.pan >= -1 && e.pan <= 1)) {
                ++rejected;
                break;
            }
            v.gain = e.gain;
            v.pan = e.pan;
            gains(v);
            break;
        }
    }

    // NativeSampleMixer::pitch_step's 32.32 step, split.
    bool set_step(Voice &v, double source_rate) {
        const double pitch = source_rate / rate_;
        if (!std::isfinite(pitch) || pitch < 1.0 / 4294967296.0 || pitch > 256.0) return false;
        const uint64_t step = uint64_t(pitch * 4294967296.0 + 0.5);
        v.step_index = uint32_t(step >> 32);
        v.step_frac = uint32_t(step);
        return true;
    }

    void note_on(Voice &v, const snd::NativeSoundSequencer::VoiceEvent &e) {
        if (e.rom >= 2 || e.bank >= 4) { ++rejected; return; }
        const snd::NativeSampleBank &bank = banks_[e.rom][e.bank];
        const auto *s = bank.sample(e.sample_index);
        Voice next;
        if (!s || !set_step(next, e.source_rate_hz) || !(e.gain >= 0 && e.gain <= 4) || !(e.pan >= -1 && e.pan <= 1)) {
            ++rejected;
            return;
        }
        Decoded *d = decoded(e.rom, e.bank, e.sample_index, bank, *s);
        stop(v);
        next.active = true;
        next.effect = !e.music;
        next.sample = d;
        next.data = d->data.data();
        ++d->playing;
        next.end = s->frames;
        next.loop = s->loop_start;
        next.looping = true;
        next.gain = e.gain;
        next.pan = e.pan;
        gains(next);
        next.level = 0;
        begin(next, Stage::Attack, attack_, kOne << 8);
        v = next;
    }
    void stop(Voice &v) {
        if (v.active && v.sample) --v.sample->playing;
        v.active = false;
        v.sample = nullptr;
    }
    // A linear ramp from the level now to `target` over `count` frames.
    static void begin(Voice &v, Stage stage, uint32_t count, int32_t target) {
        v.stage = stage;
        if (!count) {
            v.level = target;
            v.stage_left = 0;
            v.level_step = 0;
            return;
        }
        v.stage_left = count;
        v.level_step = (target - v.level) / int32_t(count);
    }
    void gains(Voice &v) const {
        constexpr float quarter_pi = 0.78539816339744830962f;
        const float angle = (v.pan + 1.0f) * quarter_pi;
        const float g = v.gain * volumes_[v.effect] * kMasterGain * 4096.0f;
        v.gain_l = v.pan == 1.0f ? 0 : int32_t(g * std::cos(angle) + 0.5f);
        v.gain_r = v.pan == -1.0f ? 0 : int32_t(g * std::sin(angle) + 0.5f);
    }

    Decoded *decoded(unsigned rom, unsigned bank, unsigned index, const snd::NativeSampleBank &b,
                     const snd::NativeSampleBank::Sample &s) {
        const uint32_t key = rom << 24 | bank << 16 | index;
        auto found = cache_.find(key);
        if (found != cache_.end()) {
            found->second.used = ++clock_;
            return &found->second;
        }
        const size_t bytes = (size_t(s.frames) + 1) * 2;
        while (cache_bytes_ + bytes > cache_limit_ && evict()) {}
        Decoded &d = cache_[key];
        d.data.resize(size_t(s.frames) + 1);
        if (reader_)
            read_sample(rom, b, s, d.data.data());
        else
            for (uint32_t f = 0; f < s.frames; ++f) d.data[f] = int16_t(std::lround(b.value(index, f) * 32768.0f));
        d.data[s.frames] = d.data[s.loop_start]; // the loop's next frame (the engine's voices all loop)
        d.used = ++clock_;
        cache_bytes_ += bytes;
        ++decodes;
        return &d;
    }
    // A sample's bytes through the reader, decoded as NativeSampleBank::value
    // decodes them (scaled to 16 bits: value * 32768 is exact, 8-bit * 256
    // and 12-bit * 16). Its logical range is the table's window (0-1 MB) and
    // then the bank's 1 MB window; silence if the read fails.
    void read_sample(unsigned rom, const snd::NativeSampleBank &b, const snd::NativeSampleBank::Sample &s,
                     int16_t *out) {
        constexpr uint32_t kWindow = 0x100000;
        const bool packed = s.format == snd::NativeSampleBank::Format::PackedSigned12;
        const uint32_t bytes = packed ? (s.frames / 2) * 3 + (s.frames % 2) * 2 : s.frames;
        scratch_.resize(bytes);
        const uint32_t start = s.byte_start, end = start + bytes;
        bool ok = true;
        if (start < kWindow) ok = reader_(rom, start, scratch_.data(), std::min(end, kWindow) - start);
        if (ok && end > kWindow) {
            const uint32_t from = std::max(start, kWindow);
            ok = reader_(rom, b.data_bank() * kWindow + (from - kWindow), scratch_.data() + (from - start), end - from);
        }
        if (!ok) {
            std::fill_n(out, s.frames, int16_t(0));
            return;
        }
        const uint8_t *p = scratch_.data();
        if (!packed) {
            for (uint32_t f = 0; f < s.frames; ++f) out[f] = int16_t(int8_t(p[f]) * 256);
            return;
        }
        for (uint32_t f = 0; f < s.frames; ++f) {
            const uint8_t *q = p + (f / 2) * 3;
            const unsigned raw = (f & 1) ? (unsigned(q[2]) << 4) | (q[1] >> 4) : (unsigned(q[0]) << 4) | (q[1] & 15);
            out[f] = int16_t((raw < 2048 ? int(raw) : int(raw) - 4096) * 16);
        }
    }
    // The least recently used sample no voice is playing. (unordered_map
    // keeps the other entries' addresses: voices hold pointers to them.)
    bool evict() {
        auto victim = cache_.end();
        for (auto it = cache_.begin(); it != cache_.end(); ++it)
            if (!it->second.playing && (victim == cache_.end() || it->second.used < victim->second.used)) victim = it;
        if (victim == cache_.end()) return false;
        cache_bytes_ -= victim->second.data.size() * 2;
        cache_.erase(victim);
        return true;
    }

    void mix(int16_t *out, size_t count) {
        for (size_t done = 0; done < count;) {
            const size_t n = std::min(count - done, acc_.size() / 2);
            std::fill_n(acc_.begin(), n * 2, 0);
            for (Voice &v : voices_)
                if (v.active) mix_voice(v, acc_.data(), n);
            limit(acc_.data(), out + done * 2, n);
            done += n;
        }
    }

    void mix_voice(Voice &v, int32_t *acc, size_t count) {
        const int16_t *data = v.data;
        for (size_t at = 0; at < count && v.active;) {
            // The envelope, once a block.
            const size_t n = std::min<size_t>(kBlock, count - at);
            if (v.stage != Stage::Sustain) {
                const uint32_t steps = std::min<uint32_t>(uint32_t(n), v.stage_left);
                v.level += v.level_step * int32_t(steps);
                v.stage_left -= steps;
                if (!v.stage_left) {
                    if (v.stage == Stage::Release) { stop(v); break; }
                    v.level = kOne << 8;
                    v.stage = Stage::Sustain;
                }
            }
            const int32_t level = std::max(0, v.level >> 8);
            const int32_t al = (level * v.gain_l) >> 15, ar = (level * v.gain_r) >> 15; // Q12
            int32_t *o = acc + at * 2;
            uint32_t index = v.index, frac = v.frac;
            const uint32_t step_index = v.step_index, step_frac = v.step_frac, end = v.end;
            for (size_t k = 0; k < n; ++k) {
                const int32_t a = data[index], b = data[index + 1];
                const int32_t s = a + (((b - a) * int32_t(frac >> 17)) >> 15); // (15 bits of the fraction)
                o[k * 2] += (s * al) >> 12;
                o[k * 2 + 1] += (s * ar) >> 12;
                const uint32_t f = frac + step_frac;
                index += step_index + (f < frac); // the carry
                frac = f;
                if (index >= end) {
                    if (!v.looping) { stop(v); break; }
                    index = v.loop + (index - end) % (end - v.loop);
                }
            }
            v.index = index;
            v.frac = frac;
            at += n;
        }
    }

    // NativeSampleMixer's peak limiter (immediate attack, 50 ms recovery,
    // one gain for both channels), then 16 bits.
    void limit(const int32_t *acc, int16_t *out, size_t count) {
        const int32_t ceiling = int32_t(kCeiling * kOne);
        for (size_t f = 0; f < count; ++f) {
            int32_t l = acc[f * 2], r = acc[f * 2 + 1];
            const int32_t peak = std::max(std::abs(l), std::abs(r));
            if (limiter_ < 1.0f || peak > ceiling) {
                limiter_ += (1.0f - limiter_) * recovery_;
                if (limiter_ >= 0.9999f) limiter_ = 1.0f;
                if (peak > ceiling) limiter_ = std::min(limiter_, float(ceiling) / float(peak));
                l = int32_t(float(l) * limiter_);
                r = int32_t(float(r) * limiter_);
            }
            out[f * 2] = int16_t(std::clamp(l, -32768, 32767));
            out[f * 2 + 1] = int16_t(std::clamp(r, -32768, 32767));
        }
    }

    std::vector<uint8_t> program_;
    std::array<std::vector<uint8_t>, 2> pcm_; // the ROMs, or their sample tables (read through reader_)
    std::array<size_t, 2> pcm_bytes_;
    PcmReader reader_;
    std::vector<uint8_t> scratch_; // a sample's bytes, read
    size_t cache_limit_;
    std::array<std::array<snd::NativeSampleBank, 4>, 2> banks_;
    uint32_t rate_;
    snd::NativeSoundSequencer sequencer_;
    std::array<Voice, kVoices> voices_{};
    std::array<int32_t, 512 * 2> acc_{};
    std::unordered_map<uint32_t, Decoded> cache_;
    size_t cache_bytes_ = 0;
    uint64_t clock_ = 0, cursor_ = 0, end_ = 0;
    int16_t *output_ = nullptr;
    size_t written_ = 0;
    uint32_t attack_ = 0, release_ = 0;
    float recovery_ = 0, limiter_ = 1.0f;
    float volumes_[2] = {1.0f, 1.0f};
};

} // namespace psp
