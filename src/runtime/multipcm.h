// Yamaha YMW-258-F "GEW8" (Sega 315-5560, MultiPCM): 28-voice sample
// player, as native C++. Transplanted from MAME's multipcm.cpp and gew.cpp
// (BSD-3-Clause, Miguel Angel Horna (ElSemi); THIRD_PARTY.md) with the
// device framework removed: the sound board calls write() on the 68000's
// register writes and generate() to render samples at clock / 224. The
// sample and envelope arithmetic is MAME's, expression for expression.
#pragma once

#include <cstdint>
#include <memory>

namespace snd {

class MultiPcm {
public:
    // clock: the chip's input clock (10 MHz on the Model 1 sound board).
    // rom: sample ROM, 4 MiB: the first MiB fixed at 0x000000, one of four
    // 1 MiB banks at 0x100000 (set_bank).
    MultiPcm(uint32_t clock, const uint8_t *rom, uint32_t rom_size);

    double sample_rate() const { return double(rate_); }
    void write(unsigned offset, uint8_t data); // 0 data, 1 slot, 2 register
    void set_bank(unsigned bank) { bank_ = bank & 3; }
    // Renders n stereo samples, each channel as MAME's put_int_clamp(v, 32768).
    void generate(float *left, float *right, int n);
    // The launcher's music and effects volumes (0..1; both 1: MAME's output,
    // bit for bit). The sound board marks the selected slot at each key-on
    // with whose voice it is (SoundBoard::voice_channel).
    void set_volumes(float music, float effects) { music_ = music, effects_ = effects; }
    void set_effect(bool effect) {
        if (cur_slot_ < uint32_t(kVoices)) slots_[cur_slot_].effect = effect;
    }

private:
    struct Sample {
        uint32_t start = 0, loop = 0, end = 0;
        uint8_t attack_reg = 0, decay1_reg = 0, decay2_reg = 0, decay_level = 0, release_reg = 0, key_rate_scale = 0;
        uint8_t lfo_vibrato_reg = 0, lfo_amplitude_reg = 0, format = 0;
    };
    enum class State : uint8_t { Attack, Decay1, Decay2, Release };
    struct Envelope {
        int32_t volume = 0;
        State state = State::Attack;
        uint8_t reverb = 0;
        int32_t attack_rate = 0, decay1_rate = 0, decay2_rate = 0, release_rate = 0, decay_level = 0;
    };
    struct Lfo {
        uint16_t phase = 0;
        uint32_t phase_step = 0;
        int32_t *table = nullptr, *scale = nullptr;
    };
    struct Slot {
        uint8_t regs[11] = {};
        bool playing = false;
        Sample sample;
        uint32_t offset = 0;
        uint8_t octave = 0;
        uint16_t pitch = 0;
        uint32_t step = 0;
        bool reverse = false;
        uint32_t pan = 0;
        uint8_t dsp_send = 0;
        uint32_t total_level = 0, dest_total_level = 0;
        int32_t total_level_step = 0;
        int32_t prev_sample = 0;
        Envelope env;
        uint8_t lfo_frequency = 0;
        Lfo pitch_lfo;
        uint8_t vibrato = 0;
        Lfo amplitude_lfo;
        uint8_t tremolo = 0;
        bool effect = false; // an effect's voice, else music's
    };

    static constexpr uint32_t TL_SHIFT = 12, EG_SHIFT = 16, LFO_SHIFT = 8;
    static constexpr int kVoices = 28;

    uint8_t read_byte(uint32_t addr) const;
    void init_sample(Sample &s, uint32_t index);
    void write_slot(Slot &slot, int32_t reg, uint8_t data);
    void retrigger_sample(Slot &slot);
    void update_step(Slot &slot);
    void lfo_init();
    void lfo_compute_step(Lfo &lfo, uint32_t lfo_frequency, uint32_t lfo_scale, int32_t amplitude_lfo);
    int32_t lfo_step(Lfo &lfo);
    int32_t envelope_generator_update(Slot &slot);
    void envelope_generator_calc(Slot &slot);
    static uint32_t get_rate(const uint32_t *steps, int32_t rate, uint32_t val);
    static uint32_t value_to_fixed(uint32_t bits, float value);

    const uint8_t *rom_;
    uint32_t rom_size_;
    unsigned bank_ = 0;
    float rate_;
    Slot slots_[kVoices];
    uint32_t cur_slot_ = 0, address_ = 0;
    float music_ = 1.0f, effects_ = 1.0f;

    uint32_t attack_step_[0x40] = {}, decay_release_step_[0x40] = {}, freq_step_table_[0x400] = {};
    int32_t left_pan_table_[0x800] = {}, right_pan_table_[0x800] = {}, linear_to_exp_volume_[0x400] = {};
    int32_t total_level_steps_[2] = {};
    int32_t pitch_table_[256] = {}, amplitude_table_[256] = {};
    int32_t pitch_scale_tables_[8][256] = {}, amplitude_scale_tables_[8][256] = {};
};

} // namespace snd
