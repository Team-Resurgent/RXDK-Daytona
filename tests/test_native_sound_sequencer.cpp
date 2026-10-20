// Synthetic, redistributable format fixtures: no game code or ROM data.
#include "runtime/native_sound_sequencer.h"

#ifdef NDEBUG
#undef NDEBUG
#endif

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <vector>

namespace {
using Sequencer = snd::NativeSoundSequencer;
using Event = Sequencer::VoiceEvent;
void put16(std::vector<uint8_t>& p, unsigned at, unsigned value) {
    p.at(at) = uint8_t(value >> 8); p.at(at + 1) = uint8_t(value);
}
void put32(std::vector<uint8_t>& p, unsigned at, unsigned value) {
    put16(p, at, value >> 16); put16(p, at + 2, value);
}
std::vector<uint8_t> fixture() {
    std::vector<uint8_t> p(0x40000);
    put32(p, 0x505a, 0x10000); put32(p, 0x505e, 0x11000);
    for (unsigned i = 0; i < 16; ++i) {
        const unsigned at = i < 10 ? 0x10000 + 10 * i : 0x11000 + 10 * (i - 10);
        p[at] = 0x80; p[at + 1] = uint8_t(i); p[at + 2] = 255;
        p[at + 7] = uint8_t(i >= 10); // Effects on the second sample ROM.
    }
    for (unsigned i = 0; i < 16; ++i) put32(p, 0x523a + i * 4, 0x14000);
    put32(p, 0x56ea, 0x15000);
    for (unsigned i = 0; i < 128; ++i) {
        p[0x5c1a + i] = 0x50; p[0x5cce + i] = uint8_t(i * 2 + 1);
        put32(p, 0x5dce + i * 4, 0x16000);
    }
    p[0x16001] = 255; p[0x16002] = 5;
    put32(p, 0x2bc6 + 4 * 0x21, 0x18000);
    put16(p, 0x18000, 0); put32(p, 0x18002, 0x19000);
    p[0x19000] = 0x80;
    const uint8_t song[] = {0x9a, 60, 127, 2, 0x8a, 60, 1, 0xff, 0x2f};
    std::copy(std::begin(song), std::end(song), p.begin() + 0x19001);
    return p;
}
struct Capture {
    std::vector<Event> events;
    static void sink(void* context, const Event& event) {
        static_cast<Capture*>(context)->events.push_back(event);
    }
    void attach(Sequencer& s) { s.set_sink(sink, this); }
};
void send(Sequencer& s, std::initializer_list<uint8_t> bytes) { s.send(bytes.begin(), bytes.size()); }
void near(double a, double b) { assert(std::abs(a - b) < 1e-5 * std::max(1., std::abs(b))); }

void test_uart_and_voices() {
    auto p = fixture(); Sequencer s(p); Capture c; c.attach(s);
    send(s, {0x90, 60}); assert(c.events.empty());
    send(s, {0xf8, 127}); assert(c.events.size() == 1);
    const auto first = c.events.back();
    assert(first.kind == Sequencer::EventKind::NoteOn && first.voice_id == 0);
    assert(first.sample_index == 5 && first.rom == 0 && first.bank == 0);
    near(first.source_rate_hz, 16. * 10000000 / 224);
    near(first.pan, 0);
    send(s, {61, 64}); assert(c.events.size() == 2); // Running status survives realtime byte.
    assert(c.events.back().voice_id == 1 && c.events.back().gain < first.gain);
    send(s, {0xb0, 0x40, 0}); // The source format's hold polarity is inverted MIDI.
    send(s, {0x80, 60, 127}); assert(c.events.size() == 2);
    send(s, {0xb0, 0x40, 127}); assert(c.events.size() == 4); // Both notes map to the fixture's same sample/pitch.
    assert(c.events.back().kind == Sequencer::EventKind::NoteOff);
    send(s, {0x90, 60, 127}); s.advance(123);
    send(s, {0xff}); assert(c.events.back().kind == Sequencer::EventKind::Stop);
    assert(c.events.back().frame == 123 && s.frames() == 123);
    assert(!s.failed() && s.stats().unsupported == 0);
}

void test_sequence_sample_clock() {
    auto p = fixture(); Sequencer a(p), b(p); Capture ca, cb; ca.attach(a); cb.attach(b);
    send(a, {0xae, 0x21, 0}); send(b, {0xae, 0x21, 0});
    a.advance(55); assert(ca.events.empty()); a.advance(1);
    assert(ca.events.size() == 1 && ca.events[0].frame == 56);
    a.advance(943); b.advance(999);
    assert(ca.events.size() == 2 && cb.events.size() == 2);
    assert(ca.events[1].kind == Sequencer::EventKind::NoteOff && ca.events[1].frame == 166);
    for (unsigned i = 0; i < ca.events.size(); ++i) {
        assert(ca.events[i].frame == cb.events[i].frame && ca.events[i].kind == cb.events[i].kind);
        assert(ca.events[i].sample_index == cb.events[i].sample_index);
    }
    assert(a.frames() == 999 && !a.failed() && a.stats().sequence_events == 2);
}

void test_playlist_and_immediate() {
    auto p = fixture(); p[0x19000] = 0; put32(p, 0x19004, 0x1a000); put32(p, 0x19008, 0xffffffff);
    const uint8_t events[] = {0x90, 60, 255, 0x80, 188, 0xff, 0x2f};
    std::copy(std::begin(events), std::end(events), p.begin() + 0x1a000);
    Sequencer s(p); Capture c; c.attach(s); send(s, {0xae, 0x21, 0}); s.advance(200);
    assert(!s.failed() && c.events.size() == 2);
    assert(c.events[0].frame == 56 && c.events[1].frame == 56);
}

void test_percussion_and_paired_metadata() {
    auto p = fixture();
    p[0x10000 + 9 * 10 + 3] = 0xf0;
    put32(p, 0x645a, 0x17000);
    p[0x17000 + 60 * 4] = 0x30; p[0x17001 + 60 * 4] = 0x50;
    p[0x17002 + 60 * 4] = 1; p[0x17003 + 60 * 4] = 7;
    Sequencer s(p); Capture c; c.attach(s); send(s, {0x99, 60, 127});
    assert(c.events.size() == 1 && c.events[0].sample_index == 7);
    const auto initial = c.events[0];
    send(s, {0xb9, 0x07, 20});
    assert(c.events.size() == 2 && c.events.back().kind == Sequencer::EventKind::Update);
    near(c.events.back().source_rate_hz, initial.source_rate_hz);
    near(c.events.back().pan, initial.pan); assert(c.events.back().gain < initial.gain);
    s.advance(14211); assert(c.events.size() == 2);
    s.advance(1); assert(c.events.size() == 3 && c.events.back().kind == Sequencer::EventKind::NoteOff);
    assert(c.events.back().frame == 14212 && !s.failed());

    auto paired = fixture();
    const uint8_t stream[] = {0xf0, 0xf7, 1, 2, 0xf7, 0, 0x9a, 60, 255, 0xff, 0x2f};
    std::copy(std::begin(stream), std::end(stream), paired.begin() + 0x19001);
    // A direct track descriptor selects its route from the first status nibble.
    paired[0x19000] = 0; put32(paired, 0x19004, 0x1a000); put32(paired, 0x19008, 0xffffffff);
    std::copy(std::begin(stream), std::end(stream), paired.begin() + 0x1a000);
    Sequencer metadata(paired); Capture cm; cm.attach(metadata);
    send(metadata, {0xae, 0x21, 0}); metadata.advance(100);
    assert(!metadata.failed() && cm.events.size() == 1 && cm.events[0].frame == 56);
}

void test_engine_routes_and_bounds() {
    auto p = fixture(); Sequencer s(p); Capture c; c.attach(s);
    send(s, {0xbe, 0x16, 5, 0x1b, 6, 0x1c, 7, 0x1d, 8, 0x1e, 9, 0x1f, 10});
    assert(c.events.empty()); send(s, {0xbe, 0x17, 1}); assert(c.events.size() == 8);
    for (const auto& event : c.events) assert(event.rom == 1 && event.voice_id >= 47);
    send(s, {0xbe, 0x50, 0x7d}); assert(c.events.size() == 8); // Transient effects stop leaves engine layers running.
    send(s, {0xbe, 0x17, 0}); assert(c.events.size() == 16);
    assert(!s.failed());
    Sequencer bad(p); send(bad, {1}); assert(bad.failed());
    auto broken = fixture(); put32(broken, 0x18002, 0x50000);
    Sequencer invalid(broken); send(invalid, {0xae, 0x21, 0}); assert(invalid.failed());
    auto loop = fixture(); loop[0x19000] = 0; put32(loop, 0x19004, 0x1a000);
    put32(loop, 0x19008, 0xfffffff1); put32(loop, 0x1900c, 0x19004);
    loop[0x1a000] = 0xff; loop[0x1a001] = 0x2f;
    Sequencer runaway(loop); send(runaway, {0xae, 0x21, 0}); runaway.advance(100);
    assert(runaway.failed() && runaway.stats().event_limit_hits == 1);
}
}

int main() {
    test_uart_and_voices(); test_sequence_sample_clock();
    test_playlist_and_immediate(); test_percussion_and_paired_metadata(); test_engine_routes_and_bounds();
    std::puts("native sound sequencer: protocol, sample-clock, playlist and bounded invalid-data tests passed");
}
