// Synthetic sample data only; no game ROM or extracted sample bytes.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "native_sample_mixer.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

namespace {
size_t allocations = 0;
}
void* operator new(size_t n) {
    ++allocations;
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }

namespace {
using Bank = snd::NativeSampleBank;
using Mixer = snd::NativeSampleMixer;

void header(std::vector<uint8_t>& rom, unsigned sample, uint32_t start,
            uint32_t frames, uint32_t loop, bool packed = false) {
    assert(frames >= 1 && frames <= 65536 && loop <= 65535);
    auto* h = rom.data() + sample * 12;
    start |= packed ? 0x400000 : 0;
    h[0] = uint8_t(start >> 16); h[1] = uint8_t(start >> 8); h[2] = uint8_t(start);
    h[3] = uint8_t(loop >> 8); h[4] = uint8_t(loop);
    const uint32_t encoded_end = 65536 - frames;
    h[5] = uint8_t(encoded_end >> 8); h[6] = uint8_t(encoded_end);
}

std::vector<uint8_t> fixture(size_t bytes = 0x400000) {
    std::vector<uint8_t> rom(bytes, 0);
    for (unsigned s = 0; s < Bank::kSamples; ++s) header(rom, s, 0, 1, 1);
    return rom;
}

void near(float actual, float expected, float tolerance = 0.000001f) {
    assert(std::isfinite(actual));
    assert(std::fabs(actual - expected) <= tolerance);
}

Mixer::VoiceParams instant() {
    Mixer::VoiceParams p;
    p.envelope.attack_seconds = 0;
    p.envelope.release_seconds = 0;
    p.pan = -1;
    return p;
}

void decoder() {
    auto rom = fixture();
    header(rom, 0, 0x2000, 4, 1);
    rom[0x2000] = 0; rom[0x2001] = 64;
    rom[0x2002] = 128; rom[0x2003] = 255;
    header(rom, 1, 0x3000, 4, 0, true);
    // Four signed 12-bit values: -2048, -1, 0, +2047.
    const uint8_t packed[] = {0x80, 0xf0, 0xff, 0x00, 0xf0, 0x7f};
    std::memcpy(rom.data() + 0x3000, packed, sizeof packed);
    header(rom, 2, 0xfffff, 3, 0);
    rom[0xfffff] = 32; rom[0x200000] = 64; rom[0x200001] = 96;
    header(rom, 3, 0x200000, 1, 0); // logical unmapped area, not physical ROM
    header(rom, 4, 0x1fffff, 2, 0); // crosses the logical window end
    header(rom, 5, 0x2000, 4, 4); // invalid loop
    header(rom, 6, 0x4000, 65536, 0);
    Bank bank(rom.data(), rom.size(), 2);
    assert(bank.valid_sample_count() == 4);
    near(bank.value(0, 0), 0); near(bank.value(0, 1), 0.5f);
    near(bank.value(0, 2), -1); near(bank.value(0, 3), -1.0f / 128);
    near(bank.value(1, 0), -1); near(bank.value(1, 1), -1.0f / 2048);
    near(bank.value(1, 2), 0); near(bank.value(1, 3), 2047.0f / 2048);
    near(bank.value(2, 0), 0.25f); near(bank.value(2, 1), 0.5f);
    near(bank.value(2, 2), 0.75f);
    assert(bank.status(3) == Bank::Status::UnmappedData);
    assert(bank.status(4) == Bank::Status::UnmappedData);
    assert(bank.status(5) == Bank::Status::InvalidLoop);
    assert(bank.sample(6)->frames == 65536);
    near(bank.value(0, 4), 0); near(bank.value(512, 0), 0);
    assert(!bank.sample(512));
    for (unsigned selected = 0; selected < 4; ++selected) {
        header(rom, 7, 0x100010, 1, 0);
        rom[selected * 0x100000 + 0x10] = uint8_t(16 * selected);
        // This modifies an unrelated invalid header in the fixed page.
        Bank view(rom.data(), rom.size(), selected);
        near(view.value(7, 0), float(selected) / 8);
        near(view.value(0, 1), 0.5f); // the fixed range never changes bank
    }
    assert(!bank.load(nullptr, rom.size(), 0));
    assert(!bank.sample(0));
    assert(!bank.load(rom.data(), 6143, 0));
    assert(!bank.load(rom.data(), rom.size(), 4));

    auto odd = fixture(0x2005);
    header(odd, 0, 0x2000, 3, 0, true);
    odd[0x2000] = 0x80; odd[0x2001] = 0xf0; odd[0x2002] = 0xff;
    odd[0x2003] = 0x7f; odd[0x2004] = 0x0f;
    Bank final(odd.data(), odd.size(), 0);
    assert(final.sample(0));
    near(final.value(0, 2), 2047.0f / 2048);
    Bank truncated(odd.data(), odd.size() - 1, 0);
    assert(truncated.status(0) == Bank::Status::UnmappedData);
    header(odd, 1, 0x100000, 1, 0);
    Bank missing_selected(odd.data(), odd.size(), 3);
    assert(!missing_selected.sample(1));
}

void exhaustive_codes_and_malformed_tables() {
    auto rom = fixture(0x5000);
    header(rom, 0, 0x2000, 256, 0);
    for (unsigned i = 0; i < 256; ++i) rom[0x2000 + i] = uint8_t(i);
    header(rom, 1, 0x3000, 4096, 0, true);
    for (unsigned i = 0; i < 4096; i += 2) {
        const unsigned address = 0x3000 + (i / 2) * 3;
        rom[address] = uint8_t(i >> 4);
        rom[address + 1] = uint8_t((i & 15) | ((i + 1) & 15) << 4);
        rom[address + 2] = uint8_t((i + 1) >> 4);
    }
    Bank bank(rom.data(), rom.size(), 0);
    for (unsigned i = 0; i < 256; ++i)
        near(bank.value(0, i), float(i < 128 ? int(i) : int(i) - 256) / 128);
    for (unsigned i = 0; i < 4096; ++i)
        near(bank.value(1, i), float(i < 2048 ? int(i) : int(i) - 4096) / 2048);
    // Deterministically exercise malformed tables, each truncated at a distinct
    // byte. Valid samples are probed at both boundaries under the sanitizers.
    uint32_t state = 0xf0359721;
    for (unsigned trial = 0; trial < 400; ++trial) {
        for (unsigned i = 0; i < 512 * 12; ++i) {
            state ^= state << 13; state ^= state >> 17; state ^= state << 5;
            rom[i] = uint8_t(state);
        }
        assert(bank.load(rom.data(), rom.size() - trial, trial % 4));
        for (unsigned s = 0; s < 512; ++s) {
            const auto* sample = bank.sample(s);
            if (!sample) continue;
            for (uint32_t f : {uint32_t(0), sample->frames - 1}) {
                const float v = bank.value(s, f);
                assert(std::isfinite(v) && v >= -1 && v < 1);
            }
            near(bank.value(s, sample->frames), 0);
            near(bank.value(s, UINT32_MAX), 0);
        }
    }
}

void playback() {
    auto rom = fixture(0x2010);
    header(rom, 0, 0x2000, 4, 1);
    rom[0x2000] = 0; rom[0x2001] = 64;
    rom[0x2002] = 128; rom[0x2003] = 127;
    Bank bank(rom.data(), rom.size(), 0);
    Mixer mixer;
    assert(mixer.set_master_gain(1));
    mixer.set_peak_limiter(false); // inspect raw interpolation, including full scale
    auto p = instant(); p.loop = false; p.pitch = 0.5;
    assert(mixer.note_on(0, bank, 0, p));
    std::array<float, 20> out;
    out.fill(9);
    mixer.render(out.data(), 10);
    const float expected[] = {0, 0.25f, 0.5f, -0.25f, -1,
        -1.0f / 256, 127.0f / 128, 127.0f / 128, 0, 0};
    for (unsigned f = 0; f < 10; ++f) {
        near(out[f * 2], expected[f]); near(out[f * 2 + 1], 0);
    }
    assert(!mixer.active(0));
    p.loop = true; p.pitch = 5;
    assert(mixer.note_on(0, bank, 0, p));
    mixer.render(out.data(), 4);
    near(out[0], 0); near(out[2], -1); near(out[4], 0.5f); near(out[6], 127.0f / 128);
    p.pitch = 1; p.start_frame = 2; p.loop_start = 2; p.loop_end = 3;
    assert(mixer.note_on(0, bank, 0, p));
    mixer.render(out.data(), 4);
    for (unsigned f = 0; f < 4; ++f) near(out[f * 2], -1);
    assert(mixer.set_pan(0, 1)); assert(mixer.set_gain(0, 0.5f));
    mixer.render(out.data(), 1); near(out[0], 0); near(out[1], -0.5f);
    assert(mixer.set_pan(0, 0));
    mixer.render(out.data(), 1);
    near(out[0], -std::sqrt(0.5f) / 2); near(out[1], out[0]);
    mixer.note_off(0); assert(!mixer.active(0));
    out.fill(9); mixer.render(out.data(), 10);
    for (float f : out) near(f, 0);
}

void envelope_and_blocks() {
    auto rom = fixture(0x2001);
    header(rom, 0, 0x2000, 1, 0); rom[0x2000] = 64;
    Bank bank(rom.data(), rom.size(), 0);
    Mixer mixer; mixer.set_master_gain(1);
    auto p = instant();
    p.envelope.attack_seconds = 4.0f / 48000;
    p.envelope.decay_seconds = 2.0f / 48000;
    p.envelope.sustain = 0.5f;
    p.envelope.release_seconds = 4.0f / 48000;
    assert(mixer.note_on(0, bank, 0, p));
    std::array<float, 32> out{};
    mixer.render(out.data(), 8);
    const float expected[] = {0.125f, 0.25f, 0.375f, 0.5f, 0.375f, 0.25f, 0.25f, 0.25f};
    for (unsigned i = 0; i < 8; ++i) near(out[i * 2], expected[i]);
    mixer.note_off(0); mixer.render(out.data(), 1);
    near(out[0], 0.1875f);
    mixer.note_off(0); // repeated gate-off must not extend the release
    mixer.render(out.data(), 4);
    near(out[0], 0.125f); near(out[2], 0.0625f); near(out[4], 0); near(out[6], 0);
    assert(!mixer.active(0));

    Mixer whole, split;
    p.pitch = 0.738921; p.pan = 0.231f;
    assert(whole.note_on(0, bank, 0, p));
    assert(split.note_on(0, bank, 0, p));
    std::array<float, 2048> a{}, b{};
    whole.render(a.data(), 1024);
    size_t offset = 0;
    for (size_t n : {size_t(1), size_t(7), size_t(64), size_t(311), size_t(641)}) {
        split.render(b.data() + 2 * offset, n); offset += n;
    }
    assert(offset == 1024 && std::memcmp(a.data(), b.data(), sizeof a) == 0);
}

// The launcher's music and effects volumes scale only their own voices, and
// at 1 and 1 change nothing.
void music_and_effects_volumes() {
    auto rom = fixture(0x2008);
    header(rom, 0, 0x2000, 8, 0);
    const uint8_t wave[] = {0, 32, 64, 96, 0, 224, 192, 160};
    std::memcpy(rom.data() + 0x2000, wave, sizeof wave);
    Bank bank(rom.data(), rom.size(), 0);
    auto music = instant(), effect = instant();
    music.pitch = 0.75; music.gain = 0.4f; music.pan = -0.5f;
    effect.pitch = 0.5; effect.gain = 0.3f; effect.pan = 0.25f; effect.effect = true;
    Mixer both, unity, music_only, effects_only, no_effects, half_music;
    for (Mixer* m : {&both, &unity, &music_only, &effects_only, &no_effects, &half_music}) m->set_peak_limiter(false);
    for (Mixer* m : {&both, &unity, &no_effects, &half_music}) assert(m->note_on(0, bank, 0, music) && m->note_on(1, bank, 0, effect));
    assert(music_only.note_on(0, bank, 0, music));
    assert(effects_only.note_on(1, bank, 0, effect));
    unity.set_volumes(1.0f, 1.0f);
    no_effects.set_volumes(1.0f, 0.0f);
    half_music.set_volumes(0.5f, 1.0f);
    std::array<float, 256> a{}, b{}, c{}, d{}, e{}, f{};
    both.render(a.data(), 128); unity.render(b.data(), 128); music_only.render(c.data(), 128);
    effects_only.render(d.data(), 128); no_effects.render(e.data(), 128); half_music.render(f.data(), 128);
    for (size_t i = 0; i < a.size(); ++i) {
        assert(a[i] == b[i]);                  // 1 and 1: bit for bit as before
        assert(e[i] == c[i]);                  // effects at 0: the music alone
        near(f[i], 0.5f * c[i] + d[i], 1e-6f); // music at half
    }
    assert(std::any_of(d.begin(), d.end(), [](float v) { return v != 0; }));
}

void default_gain_boost() {
    static_assert(Mixer::kDefaultMasterGain == 1.95f);
    auto rom = fixture(0x2008);
    header(rom, 0, 0x2000, 8, 0);
    const uint8_t wave[] = {0, 32, 64, 96, 0, 224, 192, 160};
    std::memcpy(rom.data() + 0x2000, wave, sizeof wave);
    Bank bank(rom.data(), rom.size(), 0);
    for (float pan : {-1.0f, -0.75f, 0.0f, 0.231f, 0.75f, 1.0f}) {
        for (bool shaped : {false, true}) {
            Mixer boosted, old_gain, split_boosted, split_old_gain;
            assert(old_gain.set_master_gain(0.75f));
            assert(split_old_gain.set_master_gain(0.75f));
            auto p = instant();
            p.pan = pan; p.pitch = 0.75; p.gain = 0.4f;
            if (shaped) {
                p.envelope.attack_seconds = 8.0f / Mixer::kOutputRate;
                p.envelope.decay_seconds = 16.0f / Mixer::kOutputRate;
                p.envelope.sustain = 0.5f;
                p.envelope.release_seconds = 32.0f / Mixer::kOutputRate;
            }
            for (Mixer* mixer : {&boosted, &old_gain, &split_boosted, &split_old_gain})
                assert(mixer->note_on(0, bank, 0, p));
            std::array<float, 384> actual{}, previous{}, split_actual{}, split_previous{};
            boosted.render(actual.data(), 128);
            old_gain.render(previous.data(), 128);
            size_t offset = 0;
            for (size_t n : {size_t(1), size_t(7), size_t(32), size_t(88)}) {
                split_boosted.render(split_actual.data() + 2 * offset, n);
                split_old_gain.render(split_previous.data() + 2 * offset, n);
                offset += n;
            }
            assert(offset == 128);
            for (Mixer* mixer : {&boosted, &old_gain, &split_boosted, &split_old_gain})
                mixer->note_off(0);
            boosted.render(actual.data() + 256, 64);
            old_gain.render(previous.data() + 256, 64);
            for (size_t n : {size_t(3), size_t(13), size_t(48)}) {
                split_boosted.render(split_actual.data() + 2 * offset, n);
                split_old_gain.render(split_previous.data() + 2 * offset, n);
                offset += n;
            }
            assert(offset == 192);
            assert(std::memcmp(actual.data(), split_actual.data(), sizeof actual) == 0);
            assert(std::memcmp(previous.data(), split_previous.data(), sizeof previous) == 0);
            bool positive[2]{}, negative[2]{};
            for (size_t i = 0; i < actual.size(); ++i) {
                near(actual[i], previous[i] * 2.6f);
                assert(actual[i] > -1 && actual[i] < 1);
                positive[i % 2] |= actual[i] > 0;
                negative[i % 2] |= actual[i] < 0;
                if (i >= 320) near(actual[i], 0); // completed release stays silent
            }
            for (unsigned channel = 0; channel < 2; ++channel) {
                const bool audible = channel == 0 ? pan != 1.0f : pan != -1.0f;
                assert(positive[channel] == audible && negative[channel] == audible);
            }
            for (Mixer* mixer : {&boosted, &old_gain, &split_boosted, &split_old_gain}) {
                assert(mixer->active_voices() == 0);
                assert(mixer->stats().clipped_samples == 0);
                assert(mixer->stats().limited_frames == 0);
                assert(mixer->stats().rejected_commands == 0);
            }
        }
    }
}

void peak_limiter_stereo_link() {
    static_assert(Mixer::kPeakCeiling == 0.98f);
    auto rom = fixture(0x2004);
    // Strong and weak channels have opposite signs. Swap the stronger channel
    // and both signs to check that neither channel is independently limited.
    header(rom, 0, 0x2000, 1, 0); rom[0x2000] = 64;
    header(rom, 1, 0x2001, 1, 0); rom[0x2001] = 224;
    header(rom, 2, 0x2002, 1, 0); rom[0x2002] = 192;
    header(rom, 3, 0x2003, 1, 0); rom[0x2003] = 32;
    Bank bank(rom.data(), rom.size(), 0);
    const size_t before = allocations;
    for (unsigned strong_channel : {0u, 1u}) {
        for (unsigned negative : {0u, 1u}) {
            Mixer mixer;
            auto p = instant(); p.gain = 4;
            p.pan = strong_channel ? 1.0f : -1.0f;
            assert(mixer.note_on(0, bank, negative ? 2 : 0, p));
            p.pan = -p.pan;
            assert(mixer.note_on(1, bank, negative ? 3 : 1, p));
            std::array<float, 128> output{};
            mixer.render(output.data(), 64);
            const float sign = negative ? -1.0f : 1.0f;
            for (unsigned frame = 0; frame < 64; ++frame) {
                near(output[2 * frame + strong_channel], sign * Mixer::kPeakCeiling);
                near(output[2 * frame + 1 - strong_channel], -sign * Mixer::kPeakCeiling * 0.5f);
            }
            assert(mixer.stats().limited_frames == 64);
            assert(mixer.stats().clipped_samples == 0);
            mixer.set_peak_limiter(false);
            mixer.render(output.data(), 1);
            near(output[strong_channel], sign);
            near(output[1 - strong_channel], -sign);
            assert(mixer.stats().limited_frames == 64);
            assert(mixer.stats().clipped_samples == 2);
        }
    }
    assert(allocations == before);
}

void peak_limiter_blocks_and_recovery() {
    auto rom = fixture(0x4000);
    header(rom, 0, 0x2000, 8192, 0);
    for (unsigned i = 0; i < 8192; ++i) rom[0x2000 + i] = 4;
    rom[0x2000] = 127; rom[0x2001] = 128; rom[0x2800] = 127;
    header(rom, 1, 0x2000, 1, 0);
    Bank bank(rom.data(), rom.size(), 0);
    Mixer whole, split;
    auto p = instant(); p.pan = 0.231f; p.gain = 2; p.loop = false;
    assert(whole.note_on(0, bank, 0, p));
    assert(split.note_on(0, bank, 0, p));
    std::array<float, 16384> a{}, b{};
    const size_t before = allocations;
    whole.render(a.data(), 8192);
    size_t offset = 0;
    for (size_t n : {size_t(1), size_t(7), size_t(64), size_t(311), size_t(641),
                     size_t(16), size_t(1024), size_t(2048), size_t(4080)}) {
        split.render(b.data() + 2 * offset, n); offset += n;
    }
    assert(offset == 8192 && std::memcmp(a.data(), b.data(), sizeof a) == 0);
    assert(whole.stats().limited_frames == split.stats().limited_frames);
    assert(whole.stats().limited_frames > 4096);
    assert(whole.stats().clipped_samples == 0 && split.stats().clipped_samples == 0);
    for (float value : a) assert(std::isfinite(value) && std::abs(value) <= Mixer::kPeakCeiling + 0.000001f);
    assert(a[0] > 0 && a[2] < 0); // limiter preserves both peak polarities
    for (unsigned frame = 3; frame < 2048; ++frame) assert(a[frame * 2] >= a[(frame - 1) * 2]);
    for (unsigned frame = 2050; frame < 8192; ++frame) assert(a[frame * 2] >= a[(frame - 1) * 2]);

    // Measure one 50 ms exponential recovery time constant after a loud peak.
    Mixer recovery;
    p = instant(); p.gain = 4;
    assert(recovery.note_on(0, bank, 1, p));
    std::array<float, 4800> out{};
    recovery.render(out.data(), 1);
    near(out[0], Mixer::kPeakCeiling);
    const float source = 127.0f / 128;
    const float initial_gain = Mixer::kPeakCeiling / (source * 4 * Mixer::kDefaultMasterGain);
    assert(recovery.set_gain(0, 0.125f));
    const float quiet = source * 0.125f * Mixer::kDefaultMasterGain;
    recovery.render(out.data(), 2400);
    const float expected_gain = 1.0f - (1.0f - initial_gain) * std::exp(-1.0f);
    near(out[4798], quiet * expected_gain, 0.00002f);
    assert(out[0] < out[4798] && out[4798] < quiet);

    // Zero/null rendering must not advance gain or counters.
    const auto rendered = recovery.stats().rendered_frames;
    const auto limited = recovery.stats().limited_frames;
    recovery.render(nullptr, 100);
    recovery.render(out.data(), 0);
    assert(recovery.stats().rendered_frames == rendered);
    assert(recovery.stats().limited_frames == limited);

    // Silence remains exactly silent while the existing limiter recovers.
    recovery.stop(0);
    for (unsigned block = 0; block < 20; ++block) {
        recovery.render(out.data(), 2400);
        for (float value : out) near(value, 0);
    }
    p.gain = 0.125f;
    assert(recovery.note_on(0, bank, 1, p));
    recovery.render(out.data(), 1);
    near(out[0], quiet, 0.00005f);

    // all_stop and explicit limiter changes reset attenuation immediately.
    assert(recovery.set_gain(0, 4));
    recovery.render(out.data(), 1);
    recovery.all_stop();
    assert(recovery.note_on(0, bank, 1, p));
    recovery.render(out.data(), 1);
    near(out[0], quiet);
    assert(recovery.set_gain(0, 4));
    recovery.render(out.data(), 1);
    assert(recovery.set_gain(0, 0.125f));
    recovery.set_peak_limiter(false);
    recovery.set_peak_limiter(true);
    recovery.render(out.data(), 1);
    near(out[0], quiet);
    assert(recovery.stats().clipped_samples == 0);
    assert(allocations == before);
}

void validation_and_bounded_render() {
    auto rom = fixture(0x2004);
    header(rom, 0, 0x2000, 4, 0);
    for (unsigned i = 0; i < 4; ++i) rom[0x2000 + i] = 127;
    Bank bank(rom.data(), rom.size(), 0);
    Mixer mixer;
    const size_t before = allocations;
    auto p = instant();
    for (unsigned slot = 0; slot < Mixer::kVoices; ++slot)
        assert(mixer.note_on(slot, bank, 0, p));
    assert(mixer.active_voices() == 64);
    std::array<float, 2048> out{};
    mixer.render(out.data(), 1024);
    for (unsigned i = 0; i < 1024; ++i) {
        near(out[i * 2], Mixer::kPeakCeiling); near(out[i * 2 + 1], 0);
    }
    assert(mixer.stats().limited_frames == 1024);
    assert(mixer.stats().clipped_samples == 0);
    mixer.set_peak_limiter(false);
    mixer.render(out.data(), 1024);
    for (unsigned i = 0; i < 1024; ++i) { near(out[i * 2], 1); near(out[i * 2 + 1], 0); }
    assert(mixer.stats().limited_frames == 1024);
    assert(mixer.stats().clipped_samples == 1024);
    assert(allocations == before);
    const auto rejects = mixer.stats().rejected_commands;
    p.pitch = std::numeric_limits<double>::infinity(); assert(!mixer.note_on(0, bank, 0, p));
    p = instant(); p.envelope.sustain = std::numeric_limits<float>::quiet_NaN();
    assert(!mixer.note_on(0, bank, 0, p));
    p = instant(); p.start_frame = 4; assert(!mixer.note_on(0, bank, 0, p));
    p = instant(); p.loop_start = 4; assert(!mixer.note_on(0, bank, 0, p));
    p = instant(); p.loop_end = 5; assert(!mixer.note_on(0, bank, 0, p));
    p = instant(); assert(!mixer.note_on(64, bank, 0, p));
    assert(!mixer.note_on(0, bank, 512, p));
    assert(!mixer.set_pitch(0, 0)); assert(!mixer.set_pitch(0, -1));
    assert(!mixer.set_pitch(0, 257)); assert(!mixer.set_gain(0, -1));
    assert(!mixer.set_pan(0, 2)); assert(!mixer.set_master_gain(-1));
    assert(mixer.stats().rejected_commands == rejects + 13);
    assert(mixer.active_voices() == 64); // rejected replacement leaves voice intact
    for (unsigned i = 0; i < 100; ++i) {
        assert(mixer.set_pitch(0, 1.0 / 4294967296.0));
        mixer.render(out.data(), 1024);
    }
    assert(allocations == before);
    mixer.all_stop(); assert(mixer.active_voices() == 0);
    mixer.render(out.data(), 1024);
    for (float f : out) near(f, 0);
    mixer.render(nullptr, 0);
    mixer.render(out.data(), 0);
}
} // namespace

int main() {
    decoder();
    exhaustive_codes_and_malformed_tables();
    playback();
    envelope_and_blocks();
    default_gain_boost();
    music_and_effects_volumes();
    peak_limiter_stereo_link();
    peak_limiter_blocks_and_recovery();
    validation_and_bounded_render();
    std::puts("native sample decoder/mixer: bounds, 8/12-bit, bank windows, loop/pitch, ADSR, pan, calibrated gain, music/effects volumes, stereo peak limiter, allocation-free render passed");
}
