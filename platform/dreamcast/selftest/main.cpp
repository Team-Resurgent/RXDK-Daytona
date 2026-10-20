// Dreamcast self-test: the floating-point ABI and results the game depends
// on, before any game code runs on the SH-4. The i960 code's fast paths go
// through host double (they need a real 64-bit double: -m4-single, not
// -m4-single-only) and the TGP's through host float, both bit-exact.
//
// Expected values were computed on the PC (IEEE 754, round to nearest).
// Results go to the debug console (dc-tool, or Flycast's serial log) and
// to the screen.

#include <kos.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

static_assert(sizeof(double) == 8, "build with -m4-single: the game needs a 64-bit double");
static_assert(sizeof(float) == 4);
static_assert(sizeof(void *) == 4);

namespace {

int row = 0, failures = 0;

void line(const char *text) {
    std::printf("%s\n", text);
    bfont_draw_str(vram_s + (24 + row * 26) * 640 + 24, 640, 1, text);
    ++row;
}

uint64_t bits(double v) { uint64_t b; std::memcpy(&b, &v, 8); return b; }
uint32_t bits(float v) { uint32_t b; std::memcpy(&b, &v, 4); return b; }

void check(const char *what, uint64_t got, uint64_t want) {
    char text[96];
    std::snprintf(text, sizeof text, "%-14s %016llx %s", what, (unsigned long long)got, got == want ? "ok" : "FAIL");
    if (got != want) ++failures;
    line(text);
}

// A denormal result: IEEE gives `ieee`; the SH-4 with FPSCR.DN set gives
// zero. Either is reported, neither fails here: whether the game ever depends
// on a denormal is measured on the PC (tools/ftzcheck).
void denormal(const char *what, uint64_t got, uint64_t ieee) {
    char text[96];
    std::snprintf(text, sizeof text, "%-14s %016llx %s", what, (unsigned long long)got,
                  got == ieee ? "ieee" : got == 0 ? "flushed (SH-4 DN)" : "FAIL");
    if (got != ieee && got != 0) ++failures;
    line(text);
}

} // namespace

int main() {
    // volatile: computed on the SH-4 at run time, not folded by the compiler
    volatile double one = 1.0, two = 2.0, three = 3.0, tenth = 0.1, fifth = 0.2, a = 1.1, b = 3.3;
    volatile double seven = 7.0, twentytwo = 22.0, tiny = 1e-308, small = 1e-10;
    volatile float onef = 1.0f, threef = 3.0f, twof = 2.0f, af = 1.1f, bf = 3.3f, tinyf = 1e-38f, smallf = 1e-3f;

    line("Daytona USA recomp: Dreamcast self-test");
    char text[96];
    std::snprintf(text, sizeof text, "double %u bytes, fpscr %08lx", unsigned(sizeof(double)),
                  (unsigned long)__builtin_sh_get_fpscr());
    line(text);

    check("d 1/3", bits(one / three), 0x3fd5555555555555ull);
    check("d sqrt 2", bits(std::sqrt(double(two))), 0x3ff6a09e667f3bcdull);
    check("d 0.1+0.2", bits(tenth + fifth), 0x3fd3333333333334ull);
    check("d 1.1*3.3", bits(a * b), 0x400d0a3d70a3d70aull);
    check("d 22/7", bits(twentytwo / seven), 0x4009249249249249ull);
    check("f 1/3", bits(float(onef / threef)), 0x3eaaaaabu);
    check("f 1.1*3.3", bits(float(af * bf)), 0x406851ecu);
    check("f sqrt 2", bits(std::sqrt(float(twof))), 0x3fb504f3u);
    denormal("f denormal", bits(float(tinyf * smallf)), 0x00001be0u);
    denormal("d denormal", bits(tiny * small), 0x00000000000316a2ull);

    // printf of a double: newlib's formatting, as the runtime's logs use it
    char formatted[32];
    std::snprintf(formatted, sizeof formatted, "%.6f %.3e", double(one / three), double(twentytwo / seven));
    const bool printf_ok = !std::strcmp(formatted, "0.333333 3.143e+00");
    if (!printf_ok) ++failures;
    std::snprintf(text, sizeof text, "printf double  \"%s\" %s", formatted, printf_ok ? "ok" : "FAIL");
    line(text);

    std::snprintf(text, sizeof text, "%d failure%s", failures, failures == 1 ? "" : "s");
    line(text);
    line(failures ? "SELFTEST FAIL" : "SELFTEST PASS");
    for (;;) thd_sleep(1000);
}
