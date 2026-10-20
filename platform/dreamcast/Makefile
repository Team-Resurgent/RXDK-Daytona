# The Dreamcast build (KallistiOS). Run through build_dreamcast.py, which
# sets up the KOS environment with the -m4-single ABI (a 64-bit double) and
# passes OUT, the build directory (git-ignored); nothing is written here.
#
#   make OUT=DIR selftest      the floating-point self-test (no game code)
#   make OUT=DIR videotest     the display path: a 496x384 frame through the PVR
#   make OUT=DIR NAME.cdi      either as a bootable disc image
#   make OUT=DIR GEN=DIR game-objects
#                              the runtime and the generated game code (from
#                              the host recompile, GEN = build-daytona/gen),
#                              compiled for the SH-4; game-sizes reports them

OUT ?= $(error OUT must be the build directory)
ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)

# int32_t is long in newlib for SH; the shared sources assume int (as on
# every desktop and the Vita), e.g. std::max(int32_t, int). Both are 32 bits
# here, so int32_t is made int for this build instead of changing the source.
INT32 := -U__INT32_TYPE__ -D__INT32_TYPE__=int -U__UINT32_TYPE__ '-D__UINT32_TYPE__=unsigned int' \
         -U__INT_LEAST32_TYPE__ -D__INT_LEAST32_TYPE__=int -U__UINT_LEAST32_TYPE__ '-D__UINT_LEAST32_TYPE__=unsigned int'

# The desktop build's floating-point rules: exact rounding, no contraction.
# -MMD: header dependencies, so a changed runtime header rebuilds its users.
CXXFLAGS := -std=c++20 -O2 -g -fno-fast-math -ffp-contract=off -Wall -Wextra -MMD -MP $(INT32) -I$(ROOT)/src
-include $(wildcard $(OUT)/*/*.d $(OUT)/game/*/*.d)
# The compiler is a Windows program on DreamSDK: it writes C:/... paths in the
# .d files, which this make cannot use (the colon), so header changes did not
# rebuild their users (a changed M2Board::Images left game_loop.o with the old
# layout). Rewritten as /c/... after each compile.
FIXDEP = sed -i 's|\([A-Za-z]\):/|/\L\1/|g' $(@:.o=.d)

# Keep every built file (make would delete NAME.elf and objects as
# intermediates of NAME.cdi): they are needed to debug a crash.
.SECONDARY:

# Test programs: platform/dreamcast/NAME/main.cpp, one file each.
TESTS := selftest videotest
.PHONY: $(TESTS) $(TESTS:=.cdi)
$(TESTS): %: $(OUT)/%.elf
$(TESTS:=.cdi): %.cdi: $(OUT)/%.cdi

$(OUT)/%/main.o: $(ROOT)/platform/dreamcast/%/main.cpp
	@mkdir -p $(dir $@)
	kos-c++ $(CXXFLAGS) -c $< -o $@ && $(FIXDEP)

$(OUT)/%.elf: $(OUT)/%/main.o
	kos-c++ -o $@ $^

# A bootable disc image (CDI, audio/data session layout) from NAME.elf: the
# program as 1ST_READ.BIN (binary, scrambled for the boot ROM) and a KOS
# IP.BIN bootstrap. Built from this build's own output only. (DreamSDK's
# makeip is a MinGW program: its libpng and zlib DLLs are in /mingw64/bin.)
$(OUT)/%.cdi: $(OUT)/%.elf
	rm -rf $(OUT)/$*.disc && mkdir -p $(OUT)/$*.disc
	$(KOS_OBJCOPY) -R .stack -O binary $< $(OUT)/$*.bin
	$(KOS_BASE)/utils/scramble/scramble $(OUT)/$*.bin $(OUT)/$*.disc/1ST_READ.BIN
	PATH="$$PATH:/mingw64/bin" $(KOS_BASE)/utils/makeip/makeip -f -g "DAYTONA RECOMP" -c "DAYTONA RECOMP" -n T-00000 $(OUT)/$*.ip.bin
	mkisofs -C 0,11702 -V DAYTONA -G $(OUT)/$*.ip.bin -r -J -l -o $(OUT)/$*.iso $(OUT)/$*.disc
	cdi4dc $(OUT)/$*.iso $@

# --- The game: the shared runtime and the generated code, as platform/vita --
# GEN: the host recompile's gen directory (build_dreamcast.py passes it for
# the game; the test programs do not need it).
GEN ?=
SET := daytona
SOFTFLOAT := $(ROOT)/extern/softfloat
YMFM := $(ROOT)/extern/ymfm/src
# M2_DC_MEMORY: the runtime's Dreamcast memory layout (ROM through pages,
# texture RAM supplied, a two-level page table); everything that includes the
# runtime's headers is built with it. M2_DC_SPIN_SKIP: the game's
# wait-for-the-next-frame loop skipped by whole passes (m2_board.cpp).
# M2_DC_SPEED: faster loops, the same output (tools/dcmemcheck's layer
# hashes match with it on and off).
GAME_DEFS := -DM2_ROMSET='"$(SET)"' -DM2_DC_MEMORY -DM2_DC_SPIN_SKIP -DM2_DC_SPEED -DSOFTFLOAT_FAST_INT64 -DLITTLEENDIAN=1 -DTHREAD_LOCAL= \
             -I$(SOFTFLOAT)/source/include -isystem $(YMFM)
# Generated code at -Os: 2.5 MB instead of 6.7 MB at -O2 (daytona93's,
# measured), compiled 6.5x faster.
# -O3 with unrolling for the hot C++ (runtime, frontend, TGP code): measured faster.
O3FLAGS = $(subst -O2,-O3,$(CXXFLAGS)) -funroll-loops
GEN_FLAGS := $(subst -O2,-Os,$(CXXFLAGS)) $(GAME_DEFS) -Wno-unused-label

RUNTIME := cpu tgp m2_tgp_board geo raster video m2_board game_loop zip rom_import archive m2_replay_bus \
           lockstep snd_cpu multipcm sound_board native_sample_mixer native_sound_sequencer native_sound_engine \
           enhance comm_board
# SoftFloat's file list, from the main tree's CMake list.
SOFTFLOAT_SRC := $(shell sed -n 's|.*$${SOFTFLOAT_DIR}/source/\(.*\.c\).*|\1|p' $(ROOT)/cmake/softfloat_sources.cmake)
# The i960 code: scripts/fast_gen.py's rewrite of the desktop's (fewer
# lockstep checks; build_dreamcast.py runs it and sets GEN_I960), or the
# desktop's own.
GEN_I960 ?= $(GEN)/$(SET)
GEN_SRC := $(notdir $(wildcard $(GEN_I960)/*.cpp))
# The TGP code: fast_gen.py's rewrite (its count in a local; GEN_TGP from
# build_dreamcast.py) or the desktop's own.
GEN_TGP ?= $(GEN)/$(SET)_tgp

GAME_OBJ := $(RUNTIME:%=$(OUT)/game/runtime/%.o) \
            $(addprefix $(OUT)/game/i960/,isa.o decode.o reach.o fp.o) $(OUT)/game/trace/trace.o \
            $(addprefix $(OUT)/game/ymfm/,ymfm_opn.o ymfm_adpcm.o ymfm_ssg.o) \
            $(SOFTFLOAT_SRC:%.c=$(OUT)/game/softfloat/%.o)
GEN_OBJ := $(GEN_SRC:%.cpp=$(OUT)/game/gen/%.o) $(OUT)/game/gen_tgp/tgp_gen.o $(OUT)/game/gen_snd/snd_gen.o

# Archives, made from inside game/ with relative names: the full object
# paths are too long for one Windows command line.
GAME_LIBS := $(OUT)/game/libgen.a $(OUT)/game/libruntime.a
$(OUT)/game/libgen.a: $(GEN_OBJ)
	@test -n "$(GEN)" || { echo "GEN must be the host recompile's gen directory"; exit 1; }
	cd $(OUT)/game && rm -f libgen.a && $(KOS_AR) rcs libgen.a gen/*.o gen_tgp/*.o gen_snd/*.o
$(OUT)/game/libruntime.a: $(GAME_OBJ)
	cd $(OUT)/game && rm -f libruntime.a && $(KOS_AR) rcs libruntime.a runtime/*.o i960/*.o trace/*.o ymfm/*.o \
	    softfloat/*.o softfloat/8086-SSE/*.o

# The game: the frontend (game/main.cpp) linked with both archives (they
# call each other, hence the group).
.PHONY: game game.cdi
game: $(OUT)/game.elf
game.cdi: $(OUT)/game.cdi
# game/inputs.h (the recorded input script, or none) is written by
# build_dreamcast.py into $(OUT)/game.
$(OUT)/game/main.o: $(ROOT)/platform/dreamcast/game/main.cpp
	@mkdir -p $(dir $@)
	kos-c++ $(O3FLAGS) $(GAME_DEFS) -I$(OUT)/game -c $< -o $@ && $(FIXDEP)
$(OUT)/game.elf: $(OUT)/game/main.o $(GAME_LIBS)
	kos-c++ -o $@ $< -Wl,--start-group $(GAME_LIBS) -Wl,--end-group

# The game's disc also carries rom/: the importer's images (ROMS, the host
# build's rom_cache/daytona) and the settings EEPROM and backup RAM (NVRAM),
# (A recorded input script is compiled in instead, game/inputs.h, written by
# build_dreamcast.py.)
# Built from the user's own ROM set: build output, never committed.
ROMS ?= $(error ROMS must be the importer's image directory)
NVRAM ?= $(error NVRAM must be the directory with ioboard_eeprom.bin and backup_ram.bin)
# The sound: the sound program ROM (from ROMS) and the sound pack
# (scripts/sound_pack.py, written by build_dreamcast.py).
SOUND_PACK ?= $(error SOUND_PACK must be the sound pack (build_dreamcast.py writes it))
ROM_FILES := program main_data polygons textures copro_data copro_tables
$(OUT)/game.cdi: $(OUT)/game.elf
	rm -rf $(OUT)/game.disc && mkdir -p $(OUT)/game.disc/rom
	$(KOS_OBJCOPY) -R .stack -O binary $< $(OUT)/game.bin
	$(KOS_BASE)/utils/scramble/scramble $(OUT)/game.bin $(OUT)/game.disc/1ST_READ.BIN
	cp $(ROM_FILES:%=$(ROMS)/%.bin) $(ROMS)/sound_program.bin $(NVRAM)/ioboard_eeprom.bin $(NVRAM)/backup_ram.bin $(OUT)/game.disc/rom/
	cp $(SOUND_PACK) $(OUT)/game.disc/rom/sound.pak
	PATH="$$PATH:/mingw64/bin" $(KOS_BASE)/utils/makeip/makeip -f -g "DAYTONA RECOMP" -c "DAYTONA RECOMP" -n T-00000 $(OUT)/game.ip.bin
	mkisofs -C 0,11702 -V DAYTONA -G $(OUT)/game.ip.bin -r -J -l -o $(OUT)/game.iso $(OUT)/game.disc
	cdi4dc $(OUT)/game.iso $@

.PHONY: game-objects game-sizes
game-objects: $(GAME_LIBS)
game-sizes: game-objects
	@$(KOS_CC_BASE)/bin/$(KOS_CC_PREFIX)-size -t $(OUT)/game/libgen.a | tail -1 | sed 's/(TOTALS)/generated code (i960, TGP, 68000)/'
	@$(KOS_CC_BASE)/bin/$(KOS_CC_PREFIX)-size -t $(OUT)/game/libruntime.a | tail -1 | sed 's/(TOTALS)/runtime, i960 lib, trace, ymfm, SoftFloat/'

# The geometrizer without loops turned into memcpy/memset calls: its vertex
# copies are a few words, a library call costs more (the profile's callers).
# NATIVE_GEO=1 (the default; build_dreamcast.py --no-native-geo for 0):
# M2_DC_NATIVE_GEO, the transforms by the SH-4's ftrv (the picture only; not
# bit-exact). The driver rewrites game/native_geo.txt when the setting
# changes, which rebuilds geo.o.
NATIVE_GEO ?= 1
$(OUT)/game/runtime/geo.o: $(ROOT)/src/runtime/geo.cpp $(wildcard $(OUT)/game/native_geo.txt)
	@mkdir -p $(dir $@)
	kos-c++ $(subst -O2,-O3,$(CXXFLAGS)) -funroll-loops -fno-tree-loop-distribute-patterns $(GAME_DEFS) \
	        $(if $(filter 1,$(NATIVE_GEO)),-DM2_DC_NATIVE_GEO) -c $< -o $@ && $(FIXDEP)
# The tile layers' pixel loops unrolled.
$(OUT)/game/runtime/video.o: $(ROOT)/src/runtime/video.cpp
	@mkdir -p $(dir $@)
	kos-c++ $(subst -O2,-O3,$(CXXFLAGS)) -funroll-loops $(GAME_DEFS) -c $< -o $@ && $(FIXDEP)
$(OUT)/game/runtime/%.o: $(ROOT)/src/runtime/%.cpp
	@mkdir -p $(dir $@)
	kos-c++ $(O3FLAGS) $(GAME_DEFS) -c $< -o $@ && $(FIXDEP)
$(OUT)/game/i960/%.o: $(ROOT)/src/i960/%.cpp
	@mkdir -p $(dir $@)
	kos-c++ $(CXXFLAGS) $(GAME_DEFS) -c $< -o $@ && $(FIXDEP)
$(OUT)/game/trace/%.o: $(ROOT)/src/trace/%.cpp
	@mkdir -p $(dir $@)
	kos-c++ $(CXXFLAGS) $(GAME_DEFS) -c $< -o $@ && $(FIXDEP)
$(OUT)/game/ymfm/%.o: $(YMFM)/%.cpp
	@mkdir -p $(dir $@)
	kos-c++ $(CXXFLAGS) $(GAME_DEFS) -w -c $< -o $@ && $(FIXDEP)
$(OUT)/game/softfloat/%.o: $(SOFTFLOAT)/source/%.c
	@mkdir -p $(dir $@)
	kos-cc -O2 -std=c11 -w $(INT32) $(GAME_DEFS) -DSOFTFLOAT_ROUND_ODD -DINLINE_LEVEL=5 -DSOFTFLOAT_FAST_DIV32TO16 \
	    -DSOFTFLOAT_FAST_DIV64TO32 -I$(ROOT)/platform/dreamcast/softfloat -I$(SOFTFLOAT)/source/8086-SSE -c $< -o $@
$(OUT)/game/gen/%.o: $(GEN_I960)/%.cpp
	@mkdir -p $(dir $@)
	kos-c++ $(GEN_FLAGS) -c $< -o $@ && $(FIXDEP)
# The TGP's code at -O2: small (2,024 TGP words), and at -Os its template
# helpers (alu_pre, write_reg, ...) become calls.
$(OUT)/game/gen_tgp/%.o: $(GEN_TGP)/%.cpp
	@mkdir -p $(dir $@)
	kos-c++ $(O3FLAGS) $(GAME_DEFS) -Wno-unused-label -c $< -o $@ && $(FIXDEP)
$(OUT)/game/gen_snd/%.o: $(GEN)/$(SET)_snd/%.cpp
	@mkdir -p $(dir $@)
	kos-c++ $(GEN_FLAGS) -c $< -o $@ && $(FIXDEP)
