// MultiPCM, from MAME's src/devices/sound/multipcm.cpp and gew.cpp
// (license: BSD-3-Clause, copyright-holders: Miguel Angel Horna). Framework
// (streams, save states, device_rom_interface) replaced by plain members;
// the arithmetic is unchanged.
#include "runtime/multipcm.h"

#include <algorithm>
#include <cmath>

namespace snd {

namespace {

// Times are based on a 44100Hz timebase. It's adjusted to the actual sampling rate on startup
const double BASE_TIMES[64] = {
    0,       0,       0,       0,       6222.95, 4978.37, 4148.66, 3556.01, 3111.47, 2489.21, 2074.33, 1778.00, 1555.74,
    1244.63, 1037.19, 889.02,  777.87,  622.31,  518.59,  444.54,  388.93,  311.16,  259.32,  222.27,  194.47,  155.60,
    129.66,  111.16,  97.23,   77.82,   64.85,   55.60,   48.62,   38.91,   32.43,   27.80,   24.31,   19.46,   16.24,
    13.92,   12.15,   9.75,    8.12,    6.98,    6.08,    4.90,    4.08,    3.49,    3.04,    2.49,    2.13,    1.90,
    1.72,    1.41,    1.18,    1.04,    0.91,    0.73,    0.59,    0.50,    0.45,    0.45,    0.45,    0.45};

const float LFO_FREQ[8] = {0.168f, 2.019f, 3.196f, 4.206f, 5.215f, 5.888f, 6.224f, 7.066f}; // Hz
const float PHASE_SCALE_LIMIT[8] = {0.0f, 3.378f, 5.065f, 6.750f, 10.114f, 20.170f, 40.180f, 79.307f}; // cents
const float AMPLITUDE_SCALE_LIMIT[8] = {0.0f, 0.4f, 0.8f, 1.5f, 3.0f, 6.0f, 12.0f, 24.0f}; // dB

const int32_t VALUE_TO_CHANNEL[32] = {0,  1,  2,  3,  4,  5,  6,  -1, 7,  8,  9,  10, 11, 12, 13, -1,
                                      14, 15, 16, 17, 18, 19, 20, -1, 21, 22, 23, 24, 25, 26, 27, -1};

int32_t clamp16(int32_t v) { return std::clamp(v, -32768, 32767); }

} // namespace

uint32_t MultiPcm::value_to_fixed(uint32_t bits, float value) {
    const float float_shift = float(1 << bits);
    return uint32_t(float_shift * value);
}

MultiPcm::MultiPcm(uint32_t clock, const uint8_t *rom, uint32_t rom_size) : rom_(rom), rom_size_(rom_size) {
    rate_ = float(clock) / 224;

    // Volume + pan table
    for (int32_t level = 0; level < 0x80; ++level) {
        const float vol_db = float(level) * (-24.0f) / 64.0f;
        const float total_level = std::pow(10.0f, vol_db / 20.0f) / 4.0f;
        for (int32_t pan = 0; pan < 0x10; ++pan) {
            float pan_left, pan_right;
            if (pan == 0x8) {
                pan_left = 0.0;
                pan_right = 0.0;
            } else if (pan == 0x0) {
                pan_left = 1.0;
                pan_right = 1.0;
            } else if (pan & 0x8) {
                pan_left = 1.0;
                const int32_t inverted_pan = 0x10 - pan;
                const float pan_vol_db = float(inverted_pan) * (-12.0f) / 4.0f;
                pan_right = std::pow(10.0f, pan_vol_db / 20.0f);
                if ((inverted_pan & 0x7) == 7) pan_right = 0.0;
            } else {
                pan_right = 1.0;
                const float pan_vol_db = float(pan) * (-12.0f) / 4.0f;
                pan_left = std::pow(10.0f, pan_vol_db / 20.0f);
                if ((pan & 0x7) == 7) pan_left = 0.0;
            }
            left_pan_table_[(pan << 7) | level] = int32_t(value_to_fixed(TL_SHIFT, pan_left * total_level));
            right_pan_table_[(pan << 7) | level] = int32_t(value_to_fixed(TL_SHIFT, pan_right * total_level));
        }
    }

    // Pitch steps
    for (int32_t i = 0; i < 0x400; ++i) {
        const float fcent = rate_ * (1024.0f + float(i)) / 1024.0f;
        freq_step_table_[i] = value_to_fixed(TL_SHIFT, fcent);
    }

    // Envelope steps (Times are based on 44100Hz clock, adjust to real chip clock)
    const double attack_decay_ratio = 14.32833;
    for (int32_t i = 4; i < 0x40; ++i) {
        attack_step_[i] = uint32_t(float(0x400 << EG_SHIFT) / float(BASE_TIMES[i] * 44100.0 / 1000.0));
        decay_release_step_[i] = uint32_t(float(0x400 << EG_SHIFT) / float(BASE_TIMES[i] * attack_decay_ratio * 44100.0 / 1000.0));
    }
    attack_step_[0] = attack_step_[1] = attack_step_[2] = attack_step_[3] = 0;
    attack_step_[0x3f] = 0x400 << EG_SHIFT;
    decay_release_step_[0] = decay_release_step_[1] = decay_release_step_[2] = decay_release_step_[3] = 0;

    // Total level interpolation steps
    total_level_steps_[0] = int32_t(-float(0x80 << TL_SHIFT) / (78.2f * 44100.0f / 1000.0f));    // lower
    total_level_steps_[1] = int32_t(float(0x80 << TL_SHIFT) / (78.2f * 2 * 44100.0f / 1000.0f)); // raise

    // build the linear->exponential ramps
    for (int32_t i = 0; i < 0x400; ++i) {
        const float db = -(96.0f - (96.0f * float(i) / float(0x400)));
        const float exp_volume = std::pow(10.0f, db / 20.0f);
        linear_to_exp_volume_[i] = int32_t(value_to_fixed(TL_SHIFT, exp_volume));
    }

    lfo_init();
}

uint8_t MultiPcm::read_byte(uint32_t addr) const {
    addr &= 0x3fffff; // 22 address lines
    if (addr < 0x100000) return addr < rom_size_ ? rom_[addr] : 0;
    if (addr < 0x200000) {
        const uint32_t a = bank_ * 0x100000 + (addr - 0x100000);
        return a < rom_size_ ? rom_[a] : 0;
    }
    return 0; // unmapped
}

void MultiPcm::init_sample(Sample &sample, uint32_t index) {
    const uint32_t address = index * 12;
    sample.start = (read_byte(address) << 16) | (read_byte(address + 1) << 8) | read_byte(address + 2);
    sample.format = uint8_t((sample.start >> 20) & 0xfe);
    sample.start &= 0x3fffff;
    sample.loop = (read_byte(address + 3) << 8) | read_byte(address + 4);
    sample.end = 0x10000 - ((read_byte(address + 5) << 8) | read_byte(address + 6));
    sample.attack_reg = (read_byte(address + 8) >> 4) & 0xf;
    sample.decay1_reg = read_byte(address + 8) & 0xf;
    sample.decay2_reg = read_byte(address + 9) & 0xf;
    sample.decay_level = (read_byte(address + 9) >> 4) & 0xf;
    sample.release_reg = read_byte(address + 10) & 0xf;
    sample.key_rate_scale = (read_byte(address + 10) >> 4) & 0xf;
    sample.lfo_vibrato_reg = read_byte(address + 7);
    sample.lfo_amplitude_reg = read_byte(address + 11) & 0xf;
}

void MultiPcm::retrigger_sample(Slot &slot) {
    slot.offset = 0;
    slot.prev_sample = 0;
    slot.total_level = slot.dest_total_level << TL_SHIFT;
    envelope_generator_calc(slot);
    slot.env.state = State::Attack;
    slot.env.volume = (0x3ff - 0x2a0) << EG_SHIFT;
}

void MultiPcm::update_step(Slot &slot) {
    const uint8_t oct = (slot.octave - 1) & 0xf;
    uint32_t pitch = freq_step_table_[slot.pitch];
    if (oct & 0x8) pitch >>= (16 - oct);
    else pitch <<= oct;
    slot.step = uint32_t(float(pitch) / rate_);
}

int32_t MultiPcm::envelope_generator_update(Slot &slot) {
    Envelope &e = slot.env;
    switch (e.state) {
    case State::Attack:
        e.volume += int32_t((int64_t((0x817 << (EG_SHIFT - 1)) - e.volume) * e.attack_rate) >> 24);
        if (e.volume >= (0x3ff << EG_SHIFT)) {
            e.state = State::Decay1;
            if (e.decay1_rate >= (0x400 << EG_SHIFT)) e.state = State::Decay2; // Skip DECAY1, go directly to DECAY2
            e.volume = 0x3ff << EG_SHIFT;
        }
        break;
    case State::Decay1:
        e.volume -= e.decay1_rate;
        if (e.volume <= 0) e.volume = 0;
        if (e.volume >> (EG_SHIFT + 6) <= e.decay_level) e.state = State::Decay2;
        break;
    case State::Decay2:
        e.volume -= e.decay2_rate;
        if (e.volume <= 0) e.volume = 0;
        break;
    case State::Release:
        e.volume -= e.release_rate;
        if (e.volume <= 0) {
            e.volume = 0;
            slot.playing = false;
        }
        break;
    default: return 1 << TL_SHIFT;
    }
    if (e.reverb && e.state != State::Attack && (e.volume >> EG_SHIFT) <= 0x300) {
        e.decay1_rate = int32_t(decay_release_step_[17]);
        e.decay2_rate = int32_t(decay_release_step_[17]);
        e.release_rate = int32_t(decay_release_step_[17]);
    }
    return linear_to_exp_volume_[e.volume >> EG_SHIFT];
}

uint32_t MultiPcm::get_rate(const uint32_t *steps, int32_t rate, uint32_t val) {
    if (val == 0) return steps[0];
    if (val == 0xf) return steps[0x3f];
    const int r = std::clamp(4 * int(val) + rate, 0, 0x3f);
    return steps[r];
}

void MultiPcm::envelope_generator_calc(Slot &slot) {
    int32_t octave = slot.octave;
    if (octave & 8) octave = octave - 16;
    int32_t rate;
    if (slot.sample.key_rate_scale != 0xf) rate = (octave + slot.sample.key_rate_scale) * 2 + ((slot.pitch >> 9) & 1);
    else rate = 0;
    slot.env.attack_rate = int32_t(get_rate(attack_step_, rate, slot.sample.attack_reg));
    slot.env.decay1_rate = int32_t(get_rate(decay_release_step_, rate, slot.sample.decay1_reg));
    slot.env.decay2_rate = int32_t(get_rate(decay_release_step_, rate, slot.sample.decay2_reg));
    slot.env.release_rate = int32_t(get_rate(decay_release_step_, rate, slot.sample.release_reg));
    slot.env.decay_level = 0xf - slot.sample.decay_level;
    slot.env.reverb = false;
}

void MultiPcm::lfo_init() {
    for (int32_t i = 0; i < 256; ++i) {
        if (i < 64) pitch_table_[i] = i * 2 + 128;
        else if (i < 128) pitch_table_[i] = 383 - i * 2;
        else if (i < 192) pitch_table_[i] = 384 - i * 2;
        else pitch_table_[i] = i * 2 - 383;
        if (i < 128) amplitude_table_[i] = 255 - (i * 2);
        else amplitude_table_[i] = (i * 2) - 256;
    }
    for (int32_t table = 0; table < 8; ++table) {
        float limit = PHASE_SCALE_LIMIT[table];
        for (int32_t i = -128; i < 128; ++i) {
            const float value = (limit * float(i)) / 128.0f;
            const float converted = std::pow(2.0f, value / 1200.0f);
            pitch_scale_tables_[table][i + 128] = int32_t(value_to_fixed(LFO_SHIFT, converted));
        }
        limit = -AMPLITUDE_SCALE_LIMIT[table];
        for (int32_t i = 0; i < 256; ++i) {
            const float value = (limit * float(i)) / 256.0f;
            const float converted = std::pow(10.0f, value / 20.0f);
            amplitude_scale_tables_[table][i] = int32_t(value_to_fixed(LFO_SHIFT, converted));
        }
    }
}

int32_t MultiPcm::lfo_step(Lfo &lfo) {
    lfo.phase = uint16_t(lfo.phase + lfo.phase_step);
    int32_t p = lfo.table[(lfo.phase >> LFO_SHIFT) & 0xff];
    p = lfo.scale[p];
    return p << (TL_SHIFT - LFO_SHIFT);
}

void MultiPcm::lfo_compute_step(Lfo &lfo, uint32_t lfo_frequency, uint32_t lfo_scale, int32_t amplitude_lfo) {
    const float step = LFO_FREQ[lfo_frequency] * 256.0f / rate_;
    lfo.phase_step = uint32_t(float(1 << LFO_SHIFT) * step);
    if (amplitude_lfo) {
        lfo.table = amplitude_table_;
        lfo.scale = amplitude_scale_tables_[lfo_scale];
    } else {
        lfo.table = pitch_table_;
        lfo.scale = pitch_scale_tables_[lfo_scale];
    }
}

void MultiPcm::write_slot(Slot &slot, int32_t reg, uint8_t data) {
    slot.regs[reg] = data;
    switch (reg) {
    case 0: // PANPOT
        slot.pan = (data >> 4) & 0xf;
        slot.dsp_send = data & 0xf;
        break;
    case 1: { // Sample: loads the envelope and LFO registers from its header
        init_sample(slot.sample, slot.regs[1] | ((slot.regs[2] & 1) << 8));
        slot.regs[7] = uint8_t((slot.sample.attack_reg << 4) | slot.sample.decay1_reg);
        slot.regs[8] = uint8_t((slot.sample.decay_level << 4) | slot.sample.decay2_reg);
        slot.regs[9] = uint8_t((slot.sample.key_rate_scale << 4) | slot.sample.release_reg);
        write_slot(slot, 6, slot.sample.lfo_vibrato_reg);
        write_slot(slot, 10, slot.sample.lfo_amplitude_reg);
        if (slot.playing) retrigger_sample(slot); // retrigger if key is on
        break;
    }
    case 2: // Pitch
    case 3:
        slot.octave = slot.regs[3] >> 4;
        slot.pitch = uint16_t(((slot.regs[3] & 0xf) << 6) | (slot.regs[2] >> 2));
        update_step(slot);
        break;
    case 4: // KeyOn/Off
        if (data & 0x80) {
            slot.playing = true;
            retrigger_sample(slot);
        } else if (slot.playing) {
            if (slot.sample.release_reg != 0xf) slot.env.state = State::Release;
            else slot.playing = false;
        }
        break;
    case 5: // TL + Interpolation
        slot.dest_total_level = (data >> 1) & 0x7f;
        if (!(data & 1)) { // Interpolate TL
            if ((slot.total_level >> TL_SHIFT) > slot.dest_total_level) slot.total_level_step = total_level_steps_[0]; // decrease
            else slot.total_level_step = total_level_steps_[1];                                                        // increase
        } else {
            slot.total_level = slot.dest_total_level << TL_SHIFT;
        }
        break;
    case 6:  // LFO frequency + Pitch LFO
    case 10: // Amplitude LFO
        slot.lfo_frequency = (slot.regs[6] >> 3) & 7;
        slot.vibrato = slot.regs[6] & 7;
        slot.tremolo = slot.regs[10] & 7;
        if (data) {
            lfo_compute_step(slot.pitch_lfo, slot.lfo_frequency, slot.vibrato, 0);
            lfo_compute_step(slot.amplitude_lfo, slot.lfo_frequency, slot.tremolo, 1);
        }
        break;
    case 7: // Attack rate + Decay 1 rate
    case 8: // Decay level + Decay 2 rate
    case 9: // Rate correction + Release rate
        slot.sample.attack_reg = slot.regs[7] >> 4;
        slot.sample.decay1_reg = slot.regs[7] & 0xf;
        slot.sample.decay_level = slot.regs[8] >> 4;
        slot.sample.decay2_reg = slot.regs[8] & 0xf;
        slot.sample.key_rate_scale = slot.regs[9] >> 4;
        slot.sample.release_reg = slot.regs[9] & 0xf;
        envelope_generator_calc(slot);
        break;
    }
}

void MultiPcm::write(unsigned offset, uint8_t data) {
    switch (offset) {
    case 0: // Data write
        if (address_ < 11 && cur_slot_ < uint32_t(kVoices)) write_slot(slots_[cur_slot_], int32_t(address_), data);
        break;
    case 1: cur_slot_ = uint32_t(VALUE_TO_CHANNEL[data & 0x1f]); break; // -1 (no voice): writes ignored
    case 2: address_ = data; break;
    default: break; // 0xd: effect DSP control data (not fitted on this board)
    }
}

void MultiPcm::generate(float *left, float *right, int n) {
    for (int i = 0; i < n; ++i) {
        int32_t sums[2][2] = {}; // music, effects; left, right
        for (Slot &slot : slots_) {
            if (!slot.playing) continue;
            int32_t &smpl = sums[slot.effect][0], &smpr = sums[slot.effect][1];
            const uint32_t vol = (slot.total_level >> TL_SHIFT) | (slot.pan << 7);
            uint32_t spos = slot.offset >> TL_SHIFT;
            uint32_t step = slot.step;
            int32_t csample = 0;
            const int32_t fpart = int32_t(slot.offset & ((1 << TL_SHIFT) - 1));
            if (slot.reverse) spos = slot.sample.end - spos - 1;
            if (slot.sample.format & 4) { // 12-bit linear
                const uint32_t adr = slot.sample.start + (spos >> 1) * 3;
                if (!(spos & 1)) csample = int16_t(read_byte(adr) << 8 | ((read_byte(adr + 1) & 0xf) << 4)); // ab.c ..
                else csample = int16_t((read_byte(adr + 2) << 8) | (read_byte(adr + 1) & 0xf0));             // ..C. AB
            } else {
                csample = int16_t(read_byte(slot.sample.start + spos) << 8);
            }
            int32_t sample = (csample * fpart + slot.prev_sample * ((1 << TL_SHIFT) - fpart)) >> TL_SHIFT;
            if (slot.vibrato) { // Vibrato enabled
                step = step * uint32_t(lfo_step(slot.pitch_lfo));
                step >>= TL_SHIFT;
            }
            slot.offset += step;
            if (spos ^ (slot.offset >> TL_SHIFT)) slot.prev_sample = csample;
            if (slot.offset >= (slot.sample.end << TL_SHIFT)) {
                slot.offset -= (slot.sample.end - slot.sample.loop) << TL_SHIFT;
                slot.reverse = false;
            }
            if ((slot.total_level >> TL_SHIFT) != slot.dest_total_level) slot.total_level += uint32_t(slot.total_level_step);
            if (slot.tremolo) { // Tremolo enabled
                sample = sample * lfo_step(slot.amplitude_lfo);
                sample >>= TL_SHIFT;
            }
            sample = (sample * envelope_generator_update(slot)) >> 10;
            smpl += (left_pan_table_[vol] * sample) >> TL_SHIFT;
            smpr += (right_pan_table_[vol] * sample) >> TL_SHIFT;
        }
        int32_t smpl = sums[0][0] + sums[1][0], smpr = sums[0][1] + sums[1][1];
        if (music_ != 1.0f || effects_ != 1.0f) { // the launcher's balance; at 1 and 1 the sums as they are
            smpl = int32_t(std::lround(float(sums[0][0]) * music_ + float(sums[1][0]) * effects_));
            smpr = int32_t(std::lround(float(sums[0][1]) * music_ + float(sums[1][1]) * effects_));
        }
        left[i] = float(clamp16(smpl)) / 32768.0f;
        right[i] = float(clamp16(smpr)) / 32768.0f;
    }
}

} // namespace snd
