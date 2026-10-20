// The Dreamcast's sound: the native sequencer (src/runtime/
// native_sound_sequencer, the same one the desktop's native audio uses) fed
// the bytes the game sends its sound board, its voices played by the AICA's
// hardware channels instead of a software mixer. The samples are the sound
// pack's (scripts/sound_pack.py: the ones the game plays, as AICA ADPCM),
// kept in sound RAM by a cache: sound RAM has 2 MB and the pack is larger,
// so a sample not resident is read from the disc when a note needs it (the
// least recently used samples not playing make room).
//
// Time is real time: the sequencer advances by the elapsed microseconds, so
// the music and effects play at their own speed whatever the frame rate.
// With the game slower than the arcade, sequences meet the game's commands
// at other points than on the desktop and can ask for other samples; the
// pack's list (tools/soundusage) covers sequencers running up to 4x ahead
// of the game.
#pragma once

#include "runtime/native_sound_sequencer.h"

#include <kos.h>
#include <dc/g2bus.h>
#include <dc/sound/aica_comm.h>
#include <dc/sound/sfxmgr.h>
#include <dc/sound/sound.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace dc {

class Audio {
public:
    Audio(const std::string &program_path, const std::string &pack_path)
        : program_(load(program_path)), sequencer_(program_, kRate) {
        snd_init();
        // A note off is the AICA's key off, then a fade at the channel's
        // release rate; KallistiOS's driver sets the fastest (an abrupt cut)
        // and never changes it. Rate 26 is about 18 ms for the whole fade
        // (MAME's AICA timing), near the desktop mixer's 20 ms; key-rate
        // scaling off (0xf). Register 0x14 of each channel: RR in bits 0-4.
        // (After the ARM driver's own start-up, which writes the fastest rate.)
        thd_sleep(100);
        for (unsigned ch = 0; ch < kChannels; ch++) g2_write_32(0xa0700000u + ch * 0x80u + 0x14u, (0xfu << 10) | kRelease);
        pack_ = std::fopen(pack_path.c_str(), "rb");
        if (!pack_) throw std::runtime_error("cannot open " + pack_path);
        char magic[4];
        uint32_t count = 0;
        if (std::fread(magic, 1, 4, pack_) != 4 || std::memcmp(magic, "DSP1", 4) != 0 ||
            std::fread(&count, 4, 1, pack_) != 1)
            throw std::runtime_error("not a sound pack: " + pack_path);
        entries_.resize(count);
        for (uint32_t i = 0; i < count; i++) {
            uint8_t raw[20];
            if (std::fread(raw, 1, 20, pack_) != 20) throw std::runtime_error("short sound pack");
            Entry &e = entries_[i];
            std::memcpy(&e.frames, raw + 4, 4);
            std::memcpy(&e.loop_start, raw + 8, 4);
            std::memcpy(&e.offset, raw + 12, 4);
            std::memcpy(&e.bytes, raw + 16, 4);
            uint16_t sample;
            std::memcpy(&sample, raw + 2, 2);
            index_[key(raw[0], raw[1], sample)] = int(i);
        }
        // As many as fit up front, leaving room for the ones loaded later.
        for (uint32_t i = 0; i < count && snd_mem_available() > entries_[i].bytes + kReserve; i++) resident(int(i));
        sequencer_.set_sink(event, this);
    }
    Audio(const Audio &) = delete;
    Audio &operator=(const Audio &) = delete;

    // Once a game frame, with the bytes the game sent the sound board.
    void frame(const std::vector<uint8_t> &bytes) {
        if (!bytes.empty()) sequencer_.send(bytes.data(), bytes.size());
        const uint64_t now = timer_us_gettime64();
        if (!last_us_) last_us_ = now;
        uint64_t samples = (now - last_us_) * kRate / 1000000;
        last_us_ += samples * 1000000 / kRate;
        if (samples > kRate / 4) samples = kRate / 4; // (a long stall, a disc read: a quarter second at most)
        if (samples) sequencer_.advance(size_t(samples));
    }
    unsigned loaded() const { return loaded_; }
    // Channel 0's envelope register 0x14 (release rate in bits 0-4), as the AICA has it.
    static uint32_t release_register() { return g2_read_32(0xa0700000u + 0x14u) & 0xffff; }
    uint64_t disc_loads = 0, missing = 0, failed = 0; // missing: not in the pack; failed: no room in sound RAM
    char failure[128] = {};                             // the first failed load

private:
    static constexpr uint32_t kRate = 48000;           // the sequencer's clock (its tick timing)
    static constexpr uint32_t kReserve = 96 * 1024;    // sound RAM kept free at start for later loads
    static constexpr float kMasterGain = 1.95f;        // the desktop mixer's (native_sample_mixer.h)
    static constexpr unsigned kChannels = 64;
    static constexpr uint32_t kRelease = 26;           // the AICA's release rate after a note off

    struct Entry {
        uint32_t frames = 0, loop_start = 0, offset = 0, bytes = 0;
        sfxhnd_t handle = SFXHND_INVALID;
        uint64_t used = 0;
    };

    static std::vector<uint8_t> load(const std::string &path) {
        FILE *f = std::fopen(path.c_str(), "rb");
        if (!f) throw std::runtime_error("cannot open " + path);
        std::fseek(f, 0, SEEK_END);
        std::vector<uint8_t> d(size_t(std::ftell(f)));
        std::fseek(f, 0, SEEK_SET);
        const size_t got = std::fread(d.data(), 1, d.size(), f);
        std::fclose(f);
        if (got != d.size()) throw std::runtime_error("cannot read " + path);
        return d;
    }
    static uint32_t key(unsigned rom, unsigned bank, unsigned sample) { return rom << 24 | bank << 16 | sample; }

    // The sample in sound RAM, loading it (and making room) if it is not.
    bool resident(int i) {
        Entry &e = entries_[size_t(i)];
        e.used = ++clock_;
        if (e.handle != SFXHND_INVALID) return true;
        std::vector<char> buf(e.bytes);
        if (std::fseek(pack_, long(e.offset), SEEK_SET) != 0 || std::fread(buf.data(), 1, e.bytes, pack_) != e.bytes)
            return false;
        for (;;) {
            e.handle = snd_sfx_load_raw_buf(buf.data(), e.bytes, 44100, 4, 1);
            if (e.handle != SFXHND_INVALID) break;
            if (!evict(i)) { // nothing left to evict
                if (!failure[0])
                    std::snprintf(failure, sizeof failure, "sample %d (%u bytes): %u loaded, largest free %u bytes", i,
                                  unsigned(e.bytes), loaded_, unsigned(snd_mem_available()));
                return false;
            }
        }
        ++loaded_;
        return true;
    }
    // Unload the least recently used resident sample no channel is playing.
    bool evict(int keep) {
        int victim = -1;
        for (size_t i = 0; i < entries_.size(); i++) {
            const Entry &e = entries_[i];
            if (int(i) == keep || e.handle == SFXHND_INVALID || playing(int(i))) continue;
            if (victim < 0 || e.used < entries_[size_t(victim)].used) victim = int(i);
        }
        if (victim < 0) return false;
        snd_sfx_unload(entries_[size_t(victim)].handle);
        entries_[size_t(victim)].handle = SFXHND_INVALID;
        --loaded_;
        return true;
    }
    bool playing(int i) const {
        for (int s : channel_sample_)
            if (s == i) return true;
        return false;
    }

    static int volume(float gain) {
        const float v = gain * kMasterGain * 255.0f;
        return v <= 0 ? 0 : v >= 255 ? 255 : int(v + 0.5f);
    }
    static int pan(float p) {
        const float v = (p + 1.0f) * 127.5f;
        return v <= 0 ? 0 : v >= 255 ? 255 : int(v + 0.5f);
    }

    static void event(void *self, const snd::NativeSoundSequencer::VoiceEvent &e) {
        static_cast<Audio *>(self)->apply(e);
    }
    void apply(const snd::NativeSoundSequencer::VoiceEvent &e) {
        using Kind = snd::NativeSoundSequencer::EventKind;
        const int chn = e.voice_id;
        if (chn < 0 || unsigned(chn) >= kChannels) return;
        switch (e.kind) {
        case Kind::NoteOn: {
            const auto it = index_.find(key(e.rom, e.bank, e.sample_index));
            if (it == index_.end()) { // a sample the pack does not have
                if (missing++ < 12) std::printf("GAME sound: not in the pack: rom %u bank %u sample %u (voice %u)\n",
                                                 unsigned(e.rom), unsigned(e.bank), unsigned(e.sample_index), unsigned(e.voice_id));
                return;
            }
            const bool was = entries_[size_t(it->second)].handle != SFXHND_INVALID;
            if (!resident(it->second)) { ++failed; return; }
            if (!was) ++disc_loads;
            const Entry &s = entries_[size_t(it->second)];
            sfx_play_data_t d{};
            d.chn = chn;
            d.idx = s.handle;
            d.vol = volume(e.gain);
            d.pan = pan(e.pan);
            d.loop = 1; // as the desktop mixer: from the header's loop start to the end
            d.freq = int(e.source_rate_hz + 0.5);
            d.loopstart = s.loop_start;
            d.loopend = s.frames;
            snd_sfx_play_ex(&d);
            channel_sample_[chn] = it->second;
            break;
        }
        case Kind::Update: {
            if (channel_sample_[chn] < 0) return;
            AICA_CMDSTR_CHANNEL(tmp, cmd, ch);
            cmd->cmd = AICA_CMD_CHAN;
            cmd->timestamp = 0;
            cmd->size = AICA_CMDSTR_CHANNEL_SIZE;
            cmd->cmd_id = uint32_t(chn);
            ch->cmd = AICA_CH_CMD_UPDATE | AICA_CH_UPDATE_SET_FREQ | AICA_CH_UPDATE_SET_VOL | AICA_CH_UPDATE_SET_PAN;
            ch->freq = uint32_t(e.source_rate_hz + 0.5);
            ch->vol = uint32_t(volume(e.gain));
            ch->pan = uint32_t(pan(e.pan));
            snd_sh4_to_aica(tmp, cmd->size);
            break;
        }
        case Kind::NoteOff:
        case Kind::Stop:
            if (channel_sample_[chn] < 0) return;
            snd_sfx_stop(chn);
            channel_sample_[chn] = -1;
            break;
        }
    }

    std::vector<uint8_t> program_; // the sound program ROM (the sequencer's tables and sequences)
    snd::NativeSoundSequencer sequencer_;
    FILE *pack_ = nullptr;
    std::vector<Entry> entries_;
    std::unordered_map<uint32_t, int> index_;
    int channel_sample_[kChannels] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                                      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                                      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                                      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    unsigned loaded_ = 0;
    uint64_t clock_ = 0, last_us_ = 0;
};

} // namespace dc
