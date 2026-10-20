/* SoftFloat 3e platform header for compilers without GCC's extensions
 * (MSVC, clang-cl). SoftFloat's build/Linux-x86_64-GCC/platform.h turns on
 * __int128, __builtin_clz and GNU inline rules through opts-GCC.h; MSVC
 * rejects them. Here SoftFloat uses its portable C paths instead: no
 * INLINE_LEVEL (the primitives are ordinary functions), no builtins, no
 * 128-bit integer type. Results are identical; only speed differs.
 * Selected in CMakeLists.txt. */

#define LITTLEENDIAN 1
#define INLINE static inline
