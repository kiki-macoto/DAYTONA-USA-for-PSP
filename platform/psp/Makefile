# The PSP build (pspdev: psp-gcc and PSPSDK). Run through build_psp.py, which
# passes OUT, the build directory (git-ignored); nothing is written here.
#
#   make OUT=DIR selftest      the floating-point self-test (no game code)
#   make OUT=DIR GEN=DIR ...  game: the runtime and the generated game code
#                              (from the host recompile, GEN = build-daytona/gen)
#                              as OUT/game/EBOOT.PBP; game-sizes reports them
#
# Each program is a PRX in an EBOOT.PBP (OUT/NAME/EBOOT.PBP), with MEMSIZE=1
# in its PARAM.SFO: the large-memory mode of the PSP-2000 and later (about
# 52 MB for the program instead of 24), which the game needs.

OUT ?= $(error OUT must be the build directory)
ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
PSPDEV ?= $(HOME)/pspdev
PSPSDK := $(PSPDEV)/psp/sdk
export PATH := $(PSPDEV)/bin:$(PATH)

CXX := psp-g++
CC := psp-gcc
AR := psp-gcc-ar

# int32_t is long in newlib for this target; the shared sources assume int
# (as on every desktop, the Vita and, by the same override, the Dreamcast),
# e.g. std::max(int32_t, int). Both are 32 bits here.
INT32 := -U__INT32_TYPE__ -D__INT32_TYPE__=int -U__UINT32_TYPE__ '-D__UINT32_TYPE__=unsigned int' \
         -U__INT_LEAST32_TYPE__ -D__INT_LEAST32_TYPE__=int -U__UINT_LEAST32_TYPE__ '-D__UINT_LEAST32_TYPE__=unsigned int'

# -G0: no small-data section (the generated code's globals overflow the 64 KB
# $gp window). The desktop build's floating-point rules: exact rounding, no
# contraction. -MMD: header dependencies.
PSPFLAGS := -G0 -D_PSP_FW_VERSION=600 -I$(PSPDEV)/psp/include -I$(PSPSDK)/include
CXXFLAGS := -std=c++20 -O2 -g -fno-fast-math -ffp-contract=off -Wall -Wextra -MMD -MP $(INT32) $(PSPFLAGS) -I$(ROOT)/src
-include $(wildcard $(OUT)/*/*.d $(OUT)/game/*/*.d)

LDFLAGS := -L$(PSPDEV)/psp/lib -L$(PSPSDK)/lib -specs=$(PSPSDK)/lib/prxspecs -Wl,-q,-T$(PSPSDK)/lib/linkfile.prx \
           -Wl,-zmax-page-size=128
PSPLIBS := -lpspgum -lpspgu -lpspaudio -lpsppower -lpsprtc -lpspdebug -lpspdisplay -lpspge -lpspctrl \
           -lpspsystemctrl_user -lpspkubridge -lpspsdk

.SECONDARY:

# NAME.elf -> NAME.prx -> NAME/EBOOT.PBP (title, PARAM.SFO with MEMSIZE=1).
define eboot
	psp-fixup-imports $(1).elf
	psp-prxgen $(1).elf $(1).prx
	mkdir -p $(1)
	mksfoex -d MEMSIZE=1 '$(2)' $(1).sfo
	pack-pbp $(1)/EBOOT.PBP $(1).sfo NULL NULL NULL NULL NULL $(1).prx NULL
endef

# --- The self-test: platform/psp/selftest/main.cpp, one file --------------
.PHONY: selftest
selftest: $(OUT)/selftest/EBOOT.PBP
$(OUT)/selftest/main.o: $(ROOT)/platform/psp/selftest/main.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@
$(OUT)/selftest.elf: $(OUT)/selftest/main.o
	$(CXX) $(LDFLAGS) $^ $(PSPSDK)/lib/prxexports.o $(PSPLIBS) -o $@
$(OUT)/selftest/EBOOT.PBP: $(OUT)/selftest.elf
	$(call eboot,$(OUT)/selftest,Daytona USA recomp self-test)

# --- The game: the shared runtime and the generated code, as the Dreamcast --
GEN ?=
SET := daytona
SOFTFLOAT := $(ROOT)/extern/softfloat
YMFM := $(ROOT)/extern/ymfm/src
# The Dreamcast's runtime paths, as they are (src/runtime; see
# platform/dreamcast/HANDOFF.md): M2_DC_MEMORY (ROM through pages, texture
# RAM supplied, a two-level page table), M2_DC_SPIN_SKIP (the game's
# wait-for-the-next-frame loop skipped by whole passes), M2_DC_SPEED (faster
# loops, the same output). M2_PSP_BGR: the 16-bit layers in the GE's channel
# order (video.h). M2_PSP_TEX_DIRTY: texture RAM writes recorded per 1 KB
# (m2_board), for the renderer's texture cache. M2_PSP_FAST_GEO: the
# geometrizer's per-polygon double arithmetic in float (geo.cpp; picture
# only). PROFILE_TGP=1: M2_PSP_PROFILE, the TGP's time counted
# (m2_tgp_board) for the PROFILE lines (the clock calls cost about 1 ms a
# frame themselves). M2_DC_NATIVE_GEO is the SH-4's own and is not set.
PROFILE_TGP ?= 0
GAME_DEFS := -DM2_ROMSET='"$(SET)"' -DM2_DC_MEMORY -DM2_DC_SPIN_SKIP -DM2_DC_SPEED -DM2_PSP_BGR -DM2_PSP_TEX_DIRTY \
             -DM2_PSP_FAST_GEO $(if $(filter 1,$(PROFILE_TGP)),-DM2_PSP_PROFILE) \
             -DSOFTFLOAT_FAST_INT64 -DLITTLEENDIAN=1 -DTHREAD_LOCAL= \
             -I$(SOFTFLOAT)/source/include -isystem $(YMFM)
# Generated i960 and 68000 code at -Os (size: the PSP has room for -O2, but
# the instruction cache is 16 KB); the hot C++ (runtime, frontend, TGP code)
# at -O3 with unrolling, as the Dreamcast measured faster.
O3FLAGS = $(subst -O2,-O3,$(CXXFLAGS)) -funroll-loops
GEN_FLAGS := $(subst -O2,-Os,$(CXXFLAGS)) $(GAME_DEFS) -Wno-unused-label
GEN_O ?= -Os

RUNTIME := cpu tgp m2_tgp_board geo raster video m2_board game_loop zip rom_import archive m2_replay_bus \
           lockstep snd_cpu multipcm sound_board native_sample_mixer native_sound_sequencer native_sound_engine \
           enhance comm_board
SOFTFLOAT_SRC := $(shell sed -n 's|.*$${SOFTFLOAT_DIR}/source/\(.*\.c\).*|\1|p' $(ROOT)/cmake/softfloat_sources.cmake)
# The i960 and TGP code: platform/dreamcast/scripts/fast_gen.py's rewrite
# (build_psp.py runs it and sets GEN_I960 and GEN_TGP), or the desktop's own.
GEN_I960 ?= $(GEN)/$(SET)
GEN_SRC := $(notdir $(wildcard $(GEN_I960)/*.cpp))
GEN_TGP ?= $(GEN)/$(SET)_tgp

GAME_OBJ := $(RUNTIME:%=$(OUT)/game/runtime/%.o) \
            $(addprefix $(OUT)/game/i960/,isa.o decode.o reach.o fp.o) $(OUT)/game/trace/trace.o \
            $(addprefix $(OUT)/game/ymfm/,ymfm_opn.o ymfm_adpcm.o ymfm_ssg.o) \
            $(SOFTFLOAT_SRC:%.c=$(OUT)/game/softfloat/%.o)
GEN_OBJ := $(GEN_SRC:%.cpp=$(OUT)/game/gen/%.o) $(OUT)/game/gen_tgp/tgp_gen.o $(OUT)/game/gen_snd/snd_gen.o \
           $(OUT)/game/gen_psp/gen_psp.o

GAME_LIBS := $(OUT)/game/libgen.a $(OUT)/game/libruntime.a
$(OUT)/game/libgen.a: $(GEN_OBJ)
	@test -n "$(GEN)" || { echo "GEN must be the host recompile's gen directory"; exit 1; }
	cd $(OUT)/game && rm -f libgen.a && $(AR) rcs libgen.a gen/*.o gen_tgp/*.o gen_snd/*.o gen_psp/*.o

# The memory-access helpers the generated i960 code calls (game/gen_psp.h;
# scripts/psp_gen.py writes the calls).
$(OUT)/game/gen_psp/gen_psp.o: $(ROOT)/platform/psp/game/gen_psp.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(GAME_DEFS) -c $< -o $@
$(OUT)/game/libruntime.a: $(GAME_OBJ)
	cd $(OUT)/game && rm -f libruntime.a && $(AR) rcs libruntime.a runtime/*.o i960/*.o trace/*.o ymfm/*.o \
	    softfloat/*.o softfloat/8086-SSE/*.o

.PHONY: game game-objects game-sizes runtime-objects
game: $(OUT)/game/EBOOT.PBP
game-objects: $(GAME_LIBS)
runtime-objects: $(OUT)/game/libruntime.a
game-sizes: game-objects
	@psp-size -t $(OUT)/game/libgen.a | tail -1 | sed 's/(TOTALS)/generated code (i960, TGP, 68000)/'
	@psp-size -t $(OUT)/game/libruntime.a | tail -1 | sed 's/(TOTALS)/runtime, i960 lib, trace, ymfm, SoftFloat/'

# game/inputs.h (the recorded input script, or none, and kDrawEvery) is
# written by build_psp.py into $(OUT)/game.
$(OUT)/game/main.o: $(ROOT)/platform/psp/game/main.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(O3FLAGS) $(GAME_DEFS) -I$(OUT)/game -c $< -o $@
$(OUT)/game.elf: $(OUT)/game/main.o $(GAME_LIBS)
	$(CXX) $(LDFLAGS) $< -Wl,--start-group $(GAME_LIBS) -Wl,--end-group $(PSPSDK)/lib/prxexports.o $(PSPLIBS) -o $@
$(OUT)/game/EBOOT.PBP: $(OUT)/game.elf
	$(call eboot,$(OUT)/game,Daytona USA)

# The geometrizer without loops turned into memcpy/memset calls (its vertex
# copies are a few words), as the Dreamcast.
$(OUT)/game/runtime/geo.o: $(ROOT)/src/runtime/geo.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(O3FLAGS) -fno-tree-loop-distribute-patterns $(GAME_DEFS) -c $< -o $@
$(OUT)/game/runtime/%.o: $(ROOT)/src/runtime/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(O3FLAGS) $(GAME_DEFS) -c $< -o $@
$(OUT)/game/i960/%.o: $(ROOT)/src/i960/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(GAME_DEFS) -c $< -o $@
$(OUT)/game/trace/%.o: $(ROOT)/src/trace/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(GAME_DEFS) -c $< -o $@
$(OUT)/game/ymfm/%.o: $(YMFM)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(GAME_DEFS) -w -c $< -o $@
$(OUT)/game/softfloat/%.o: $(SOFTFLOAT)/source/%.c
	@mkdir -p $(dir $@)
	$(CC) -O2 -std=c11 -w -G0 $(INT32) $(GAME_DEFS) -DSOFTFLOAT_ROUND_ODD -DINLINE_LEVEL=5 -DSOFTFLOAT_FAST_DIV32TO16 \
	    -DSOFTFLOAT_FAST_DIV64TO32 -I$(ROOT)/platform/psp/softfloat -I$(SOFTFLOAT)/source/8086-SSE -c $< -o $@
$(OUT)/game/gen/%.o: $(GEN_I960)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(subst -Os,$(GEN_O),$(GEN_FLAGS)) -c $< -o $@
# The TGP's code at -O2: at -Os its template helpers become calls (1,627 of
# them; 91 KB), and -O3 with unrolling, the Dreamcast's choice (Flycast
# models no cache), is the largest (166 KB, 369 calls against -O2's 142 KB
# and 246). The console's game frame is instruction-cache bound.
$(OUT)/game/gen_tgp/%.o: $(GEN_TGP)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(GAME_DEFS) -Wno-unused-label -c $< -o $@
$(OUT)/game/gen_snd/%.o: $(GEN)/$(SET)_snd/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(GEN_FLAGS) -c $< -o $@
