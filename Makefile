# ReRed Faction II build. Host:  make            (x86-64, for bring-up)
#                   Device: make ARCH=aarch64 (inside docker rered-cross:bookworm)
ARCH   ?= host
GEN    ?= build/gen
OUT    := build/$(ARCH)
ifeq ($(ARCH),aarch64)
CC     := aarch64-linux-gnu-gcc
TUNE   := -mcpu=cortex-a53
else
CC     ?= cc
TUNE   :=
endif
OPT    ?= -O2
MEMBASE ?= 0x100000000ull
PC     := third_party/pcrecomp
CFLAGS := $(if $(filter 1,$(ICALL_TRACE)),-DRECOMP_ICALL_TRACE) $(if $(filter 1,$(TRACK_CUR_FUNC)),-DRECOMP_TRACK_CUR_FUNC) $(OPT) -g1 $(TUNE) -fno-strict-aliasing -fwrapv -DRECOMP_MEM_BASE=$(MEMBASE) \
          -I$(PC)/recomp32 -I$(PC)/win32hle -Iruntime $(shell pkg-config --cflags sdl2 SDL2_ttf)
LOCAL_REGS ?= 1
NONVOLATILE_MEM ?= 1
ICALL_TRACE ?= 0
TRACK_CUR_FUNC ?= 0
GENFLAGS := $(CFLAGS) -w $(if $(filter 1,$(LOCAL_REGS)),-DRECOMP_LOCAL_REGS) $(if $(filter 1,$(NONVOLATILE_MEM)),-DRECOMP_NONVOLATILE_MEM)
LIBS   := $(shell pkg-config --libs sdl2 SDL2_ttf) -lpthread -lm -ldl
# minimal static FFmpeg (scripts/build_ffmpeg.sh): Bink movies, Miles MP3
FFMPEG := build/ffmpeg/$(ARCH)
ifneq ($(wildcard $(FFMPEG)/lib/libavcodec.a),)
CFLAGS += -DHLE_WITH_FFMPEG -I$(FFMPEG)/include
LIBS   := $(FFMPEG)/lib/libavformat.a $(FFMPEG)/lib/libavcodec.a $(FFMPEG)/lib/libswresample.a $(FFMPEG)/lib/libavutil.a $(LIBS)
endif

HLE_SRC := $(filter-out %_selftest.c,$(wildcard $(PC)/win32hle/*.c))
RT_EXCLUDE ?=
RT_SRC  := $(filter-out $(RT_EXCLUDE),$(wildcard runtime/*.c))
GEN_SRC := $(wildcard $(GEN)/*/f_*.c) $(GEN)/dispatch.c

obj = $(patsubst %.c,$(OUT)/%.o,$(1))
OBJS := $(call obj,$(HLE_SRC)) $(call obj,$(RT_SRC)) $(call obj,$(GEN_SRC))

$(OUT)/rered: $(OBJS)
	$(CC) -o $@ $^ $(LIBS)

$(OUT)/$(GEN)/%.o: $(GEN)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(GENFLAGS) -MMD -MP -I$(dir $<) -c $< -o $@

$(OUT)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Wall -MMD -MP -c $< -o $@

clean:
	rm -rf $(OUT)
.PHONY: clean

-include $(OBJS:.o=.d)
