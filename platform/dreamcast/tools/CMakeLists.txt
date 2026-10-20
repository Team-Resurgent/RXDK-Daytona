cmake_minimum_required(VERSION 3.20)
# Host tools for the Dreamcast port, built with the desktop compiler from the
# main tree's runtime and the host-generated game code, unchanged. A separate
# project, like platform/vita: configured by platform/dreamcast/build_dreamcast.py
# after the desktop build and scripts/recompile.py have run.
#
# DAYTONA_HOST_BUILD (the desktop build directory): link the runtime and game
# code that build already compiled (its libraries and m2run's objects), so
# only the tools themselves are compiled. Without it, or when that build is
# incomplete, everything is compiled here from the same sources.
project(daytona_dreamcast_tools LANGUAGES C CXX)
get_filename_component(DAYTONA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# The desktop build's flags (the root CMakeLists.txt).
if(MSVC)
    add_compile_options(/W4 /fp:strict /bigobj /utf-8)
else()
    add_compile_options(-Wall -Wextra -fno-fast-math -ffp-contract=off)
endif()

set(M2_ROMSET daytona CACHE STRING "ROM set the generated code is from (the port uses daytona, Revision A)")
set(DAYTONA_GEN_ROOT "${DAYTONA_ROOT}/build-daytona/gen" CACHE PATH "Host-generated i960, TGP and 68000 C++ root")
set(DAYTONA_HOST_BUILD "" CACHE PATH "Desktop build directory to reuse the compiled runtime and game code from")
foreach(path ${M2_ROMSET}/gen_table.cpp ${M2_ROMSET}_tgp/tgp_gen.cpp ${M2_ROMSET}_snd/snd_gen.cpp)
    if(NOT EXISTS "${DAYTONA_GEN_ROOT}/${path}")
        message(FATAL_ERROR "Missing ${DAYTONA_GEN_ROOT}/${path}. Run scripts/recompile.py for ${M2_ROMSET} first; no game code is in the repository.")
    endif()
endforeach()
file(GLOB GEN_SOURCES CONFIGURE_DEPENDS "${DAYTONA_GEN_ROOT}/${M2_ROMSET}/*.cpp")
list(APPEND GEN_SOURCES "${DAYTONA_GEN_ROOT}/${M2_ROMSET}_tgp/tgp_gen.cpp" "${DAYTONA_GEN_ROOT}/${M2_ROMSET}_snd/snd_gen.cpp")

# --- The desktop build's compiled runtime and game code ----------------------
set(HOST_FILES)
if(DAYTONA_HOST_BUILD)
    foreach(name runtime i960 trace ymfm softfloat lzma7z)
        set(found)
        foreach(candidate "Release/${name}.lib" "${name}.lib" "lib${name}.a")
            if(NOT found AND EXISTS "${DAYTONA_HOST_BUILD}/${candidate}")
                set(found "${DAYTONA_HOST_BUILD}/${candidate}")
            endif()
        endforeach()
        if(found)
            list(APPEND HOST_LIBS "${found}")
        elseif(NOT name STREQUAL "lzma7z") # optional: only when 7z support was built
            message(STATUS "Host build ${DAYTONA_HOST_BUILD}: no ${name} library; compiling here")
            set(HOST_LIBS)
            break()
        endif()
    endforeach()
    # m2run's objects of the generated code (Visual Studio: m2run.dir/Release;
    # Ninja and Makefiles: CMakeFiles/m2run.dir/...).
    file(GLOB_RECURSE HOST_OBJECTS "${DAYTONA_HOST_BUILD}/m2run.dir/*" "${DAYTONA_HOST_BUILD}/CMakeFiles/m2run.dir/*")
    list(FILTER HOST_OBJECTS INCLUDE REGEX "/(chunk_[0-9]+|gen_table|tgp_gen|snd_gen)\\.(cpp\\.)?(obj|o)$")
    list(LENGTH GEN_SOURCES n_sources)
    list(LENGTH HOST_OBJECTS n_objects)
    if(HOST_LIBS AND n_objects EQUAL n_sources)
        set(HOST_FILES ${HOST_OBJECTS} ${HOST_LIBS})
        message(STATUS "Reusing ${DAYTONA_HOST_BUILD}: ${n_objects} game-code objects and its runtime libraries")
    elseif(HOST_LIBS)
        message(STATUS "Host build ${DAYTONA_HOST_BUILD}: ${n_objects} of ${n_sources} game-code objects; compiling here")
    endif()
endif()

add_library(dc_game INTERFACE)
target_include_directories(dc_game INTERFACE "${DAYTONA_ROOT}/src")
target_include_directories(dc_game SYSTEM INTERFACE "${DAYTONA_ROOT}/extern/ymfm/src"
    "${DAYTONA_ROOT}/extern/softfloat/source/include")
target_compile_definitions(dc_game INTERFACE M2_ROMSET="${M2_ROMSET}" $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>)
if(HOST_FILES)
    target_link_libraries(dc_game INTERFACE ${HOST_FILES})
else()
    # --- Or compiled here, as platform/vita does --------------------------------
    set(SOFTFLOAT_DIR "${DAYTONA_ROOT}/extern/softfloat")
    set(YMFM_DIR "${DAYTONA_ROOT}/extern/ymfm/src")
    foreach(path "${SOFTFLOAT_DIR}/source/softfloat_state.c" "${YMFM_DIR}/ymfm_opn.cpp")
        if(NOT EXISTS "${path}")
            message(FATAL_ERROR "Missing dependency ${path}. Run python3 scripts/setup.py first.")
        endif()
    endforeach()
    include("${DAYTONA_ROOT}/cmake/softfloat_sources.cmake")
    add_library(dc_softfloat STATIC ${SOFTFLOAT_SOURCES})
    if(MSVC)
        set(SOFTFLOAT_PLATFORM "${DAYTONA_ROOT}/cmake/softfloat-portable")
        set(SOFTFLOAT_TLS_C "__declspec(thread)")
        set(SOFTFLOAT_TLS_CXX thread_local)
        set(SOFTFLOAT_INLINE)
    else()
        set(SOFTFLOAT_PLATFORM "${SOFTFLOAT_DIR}/build/Linux-x86_64-GCC")
        set(SOFTFLOAT_TLS_C _Thread_local)
        set(SOFTFLOAT_TLS_CXX __thread)
        set(SOFTFLOAT_INLINE INLINE_LEVEL=5)
        target_compile_options(dc_softfloat PRIVATE -w)
    endif()
    target_include_directories(dc_softfloat PUBLIC "${SOFTFLOAT_DIR}/source/include"
        PRIVATE "${SOFTFLOAT_PLATFORM}" "${SOFTFLOAT_DIR}/source/8086-SSE")
    target_compile_definitions(dc_softfloat
        PUBLIC SOFTFLOAT_FAST_INT64 LITTLEENDIAN=1
            $<$<COMPILE_LANGUAGE:C>:THREAD_LOCAL=${SOFTFLOAT_TLS_C}>
            $<$<COMPILE_LANGUAGE:CXX>:THREAD_LOCAL=${SOFTFLOAT_TLS_CXX}>
        PRIVATE SOFTFLOAT_ROUND_ODD ${SOFTFLOAT_INLINE} SOFTFLOAT_FAST_DIV32TO16 SOFTFLOAT_FAST_DIV64TO32)
    set_target_properties(dc_softfloat PROPERTIES C_STANDARD 11)

    add_library(dc_i960 STATIC
        "${DAYTONA_ROOT}/src/i960/isa.cpp" "${DAYTONA_ROOT}/src/i960/decode.cpp"
        "${DAYTONA_ROOT}/src/i960/reach.cpp" "${DAYTONA_ROOT}/src/i960/fp.cpp")
    target_include_directories(dc_i960 PUBLIC "${DAYTONA_ROOT}/src")
    target_link_libraries(dc_i960 PUBLIC dc_softfloat)
    add_library(dc_trace STATIC "${DAYTONA_ROOT}/src/trace/trace.cpp")
    target_include_directories(dc_trace PUBLIC "${DAYTONA_ROOT}/src")
    add_library(dc_ymfm STATIC "${YMFM_DIR}/ymfm_opn.cpp" "${YMFM_DIR}/ymfm_adpcm.cpp" "${YMFM_DIR}/ymfm_ssg.cpp")
    target_include_directories(dc_ymfm SYSTEM PUBLIC "${YMFM_DIR}")
    if(NOT MSVC)
        target_compile_options(dc_ymfm PRIVATE -w)
    endif()

    # The same native runtime as the desktop; 7z is left out (the tools read
    # the importer's images, not archives).
    set(RUNTIME_FILES cpu tgp m2_tgp_board geo raster video m2_board game_loop zip rom_import
        archive m2_replay_bus lockstep snd_cpu multipcm sound_board native_sample_mixer native_sound_sequencer
        native_sound_engine enhance comm_board)
    set(RUNTIME_SOURCES)
    foreach(name IN LISTS RUNTIME_FILES)
        list(APPEND RUNTIME_SOURCES "${DAYTONA_ROOT}/src/runtime/${name}.cpp")
    endforeach()
    add_library(dc_runtime STATIC ${RUNTIME_SOURCES})
    target_include_directories(dc_runtime PUBLIC "${DAYTONA_ROOT}/src")
    target_link_libraries(dc_runtime PUBLIC dc_trace dc_i960 dc_ymfm)
    target_compile_definitions(dc_runtime PUBLIC M2_ROMSET="${M2_ROMSET}")

    # An object library: the generated code and the runtime call each other.
    add_library(dc_gen OBJECT ${GEN_SOURCES})
    target_link_libraries(dc_gen PUBLIC dc_runtime)
    if(NOT MSVC)
        target_compile_options(dc_gen PRIVATE -Wno-unused-label)
    endif()
    target_link_libraries(dc_game INTERFACE dc_gen dc_runtime)
endif()

# --- The tools ----------------------------------------------------------------
# ROM usage: which pages of each ROM region the game reads, and when
# (Windows guard pages; see romuse/main.cpp).
if(WIN32)
    add_executable(romuse romuse/main.cpp)
    target_link_libraries(romuse PRIVATE dc_game)
endif()

# Does flushing denormals (as the SH-4 does) change the game? A replay run
# normally and with SSE flush-to-zero/denormals-are-zero, compared per frame.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64|i.86")
    add_executable(ftzcheck ftzcheck/main.cpp)
    target_link_libraries(ftzcheck PRIVATE dc_game)
endif()

# The desktop's side of a lockstep check with the Dreamcast: per frame, i960
# and TGP counts and the TGP buffer RAM's hash (see tracecheck/main.cpp).
add_executable(tracecheck tracecheck/main.cpp)
target_link_libraries(tracecheck PRIVATE dc_game)

# Where the i960 spends its instructions: IP samples every 61 instructions
# (see ipprof/main.cpp).
add_executable(ipprof ipprof/main.cpp)
target_link_libraries(ipprof PRIVATE dc_game)

# Which samples the game plays: the native sound sequencer over input scripts
# (see soundusage/main.cpp), for the Dreamcast's sound pack.
add_executable(soundusage soundusage/main.cpp)
target_link_libraries(soundusage PRIVATE dc_game)

# dcmemcheck: the runtime built with M2_DC_MEMORY (the Dreamcast's memory
# layout) on the desktop, ROM pages served from the images, against the
# desktop's game code objects (which do not include the headers
# M2_DC_MEMORY changes) and its other libraries. Reuse mode only.
# DC_SPEED off: dcmemcheck without M2_DC_SPEED, to compare its layer hashes
# with the default build (the speed changes must not change a pixel).
option(DC_SPEED "dcmemcheck with M2_DC_SPEED" ON)
if(HOST_FILES)
    set(DCMEM_SOURCES)
    foreach(name cpu tgp m2_tgp_board geo raster video m2_board game_loop zip rom_import archive m2_replay_bus
            lockstep snd_cpu multipcm sound_board native_sample_mixer native_sound_sequencer native_sound_engine
            enhance comm_board)
        list(APPEND DCMEM_SOURCES "${DAYTONA_ROOT}/src/runtime/${name}.cpp")
    endforeach()
    add_library(dc_runtime_dcmem STATIC ${DCMEM_SOURCES})
    target_include_directories(dc_runtime_dcmem PUBLIC "${DAYTONA_ROOT}/src")
    target_include_directories(dc_runtime_dcmem SYSTEM PUBLIC "${DAYTONA_ROOT}/extern/ymfm/src"
        "${DAYTONA_ROOT}/extern/softfloat/source/include")
    target_compile_definitions(dc_runtime_dcmem PUBLIC M2_ROMSET="${M2_ROMSET}" M2_DC_MEMORY M2_DC_SPIN_SKIP
        $<$<BOOL:${DC_SPEED}>:M2_DC_SPEED>
        $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>)
    set(HOST_OTHER_LIBS ${HOST_LIBS})
    list(FILTER HOST_OTHER_LIBS EXCLUDE REGEX "runtime\.(lib|a)$")
    add_executable(dcmemcheck dcmemcheck/main.cpp)
    target_link_libraries(dcmemcheck PRIVATE dc_runtime_dcmem ${HOST_OBJECTS} ${HOST_OTHER_LIBS})

    # dcfastcheck: dcmemcheck on the Dreamcast's rewritten i960 code
    # (scripts/fast_gen.py's output in DC_FAST_GEN), compiled here with the
    # runtime's Dreamcast defines; the TGP and 68000 code from the host build.
    set(DC_FAST_GEN "" CACHE PATH "fast_gen.py output for M2_ROMSET (build-daytona/dreamcast/gen_fast/daytona)")
    if(DC_FAST_GEN)
        file(GLOB FAST_SOURCES CONFIGURE_DEPENDS "${DC_FAST_GEN}/*.cpp")
        set(HOST_TGP_SND ${HOST_OBJECTS})
        list(FILTER HOST_TGP_SND INCLUDE REGEX "/(tgp_gen|snd_gen)\.(cpp\.)?(obj|o)$")
        add_executable(dcfastcheck dcmemcheck/main.cpp ${FAST_SOURCES})
        target_compile_options(dcfastcheck PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/bigobj>)
        target_link_libraries(dcfastcheck PRIVATE dc_runtime_dcmem ${HOST_TGP_SND} ${HOST_OTHER_LIBS})
    endif()
endif()

# The Dreamcast controller mapping (game/controls.h, KOS-free) on the PC.
add_executable(test_controls test_controls.cpp)
