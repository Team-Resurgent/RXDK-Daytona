// The Model 1 sound board as native runtime (design doc, sound): the
// recompiled 68000 program (tools/m2sndrecomp) on its native context, with
// the board's devices as native code: the UART receiving the i960's command
// bytes, the YM3438 (ymfm, BSD-3) and two MultiPCMs (MAME's, transplanted).
//
// No cycle clock. Time is counted in completed 68000 instructions, at the
// rate MAME's 68000 runs this program (kIps); events land on that count: a
// command byte arriving at the UART's line rate, a YM timer expiring. The
// chips are rendered up to the current count before each register write, so
// every write takes effect on the right sample. The caller advances the
// board a video frame at a time and drains the rendered audio.
#pragma once

#include "runtime/multipcm.h"
#include "runtime/snd_gen_support.h"

#include "ymfm_opn.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <utility>
#include <vector>

namespace snd {

class SoundBoard : public Devices, public ymfm::ymfm_interface {
public:
    // 68000 instructions per second of board time: MAME's 68000 (10 MHz)
    // runs 15.68M instructions of this program in 1200 frames at 57.5242 Hz.
    static constexpr uint64_t kIps = 752000;
    static constexpr uint32_t kYmClock = 8000000, kPcmClock = 10000000;
    static constexpr uint32_t kBaud = 31250;          // MIDI-rate serial line from the i960
    static constexpr uint32_t kByteBits = 10;         // start, 8 data, stop

    SoundBoard(const std::vector<uint8_t> &program, const std::vector<uint8_t> &pcm1, const std::vector<uint8_t> &pcm2);

    // Command bytes the i960 wrote to its UART; they arrive one line-time
    // apart from now on.
    void send(const uint8_t *bytes, size_t n);
    // Run the board for `seconds` of board time.
    void advance(double seconds);

    // Rendered audio since the last take: interleaved stereo floats, already
    // at the board's mix gains (YM 0.30, each MultiPCM 0.50).
    double fm_rate() const { return double(kYmClock) / 144; }
    double pcm_rate() const { return double(kPcmClock) / 224; }
    std::vector<float> take_fm() { return std::exchange(fm_out_, {}); }
    std::vector<float> take_pcm() { return std::exchange(pcm_out_, {}); }

    uint64_t instructions() const { return sched_.count; }
    // The launcher's music and effects volumes (0..1; both 1: unchanged).
    void set_volumes(float music, float effects) { pcm1_.set_volumes(music, effects), pcm2_.set_volumes(music, effects); }
    // Which driver channel owns the voice on MultiPCM `chip` (0, 1) at slot
    // code `slot` (the value the driver writes to the slot register), from
    // the driver's voice pools in its RAM; -1 if none does (the engine's
    // fixed slots).
    int voice_channel(unsigned chip, uint8_t slot) const;
    const std::vector<uint8_t> &ram() const { return cpu_.ram; } // the 68000's work RAM, 0xf00000 on (tests)
    size_t bytes_received() const { return received_; }

    // Devices (the 68000's view)
    uint8_t read(uint32_t addr) override;
    void write(uint32_t addr, uint16_t data) override;

    // ymfm_interface
    void ymfm_set_timer(uint32_t tnum, int32_t duration_in_clocks) override;

private:
    void run_to(uint64_t count);
    void render_fm_to(uint64_t count);
    void render_pcm_to(uint64_t count);
    uint64_t ym_clock_at(uint64_t count) const { return count * kYmClock / kIps; }
    void rx_arrive(uint8_t byte);

    std::vector<uint8_t> program_, pcm1_rom_, pcm2_rom_;
    Cpu68k cpu_;
    Sched sched_{cpu_};
    sndgen::Env env_{cpu_, sched_};
    double carry_ = 0; // fractional instructions from advance()

    // UART (i8251, receive side)
    bool expect_mode_ = true, rx_enable_ = false, rxrdy_ = false;
    uint8_t rx_hold_ = 0, errors_ = 0;
    uint64_t line_free_ = 0; // count at which the serial line is free for the next byte
    size_t received_ = 0;

    // YM3438
    ymfm::ym3438 ym_{*this};
    uint64_t fm_done_ = 0;
    uint64_t timer_gen_[2] = {}, timer_due_clk_[2] = {};
    int in_timer_ = -1;
    std::vector<float> fm_out_;

    // MultiPCMs
    MultiPcm pcm1_, pcm2_;
    uint8_t pcm_slot_[2] = {}, pcm_register_[2] = {}; // what each chip's driver writes are aimed at
    uint64_t pcm_done_ = 0;
    std::vector<float> pcm_out_;
};

} // namespace snd
