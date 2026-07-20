# Tigerbyte - Tiger Game.com libretro core
#
# PC build (this is the primary dev/test target for now):
#   Windows (MSYS2 mingw64):  mingw32-make
#   Linux:                    make platform=unix
#
# Android / other targets are wired in later phases (see local-notes/roadmap.md).

TARGET_NAME := tigerbyte

CC      ?= gcc
CFLAGS  := -O2 -Wall -Wextra -std=c99 -Isrc
LDFLAGS := -shared

SOURCES := src/tigerbyte_libretro.c \
           src/cpu/sm8521.c \
           src/sys/gcbus.c \
           src/sys/ppu.c \
           src/sys/sound.c \
           src/sys/gcsystem.c
OBJECTS := $(SOURCES:.c=.o)

# Every object depends on every header: struct layouts (gcbus_t/gcsystem_t)
# cross TU boundaries, and an incremental build mixing pre- and post-layout
# objects links fine but corrupts state at runtime.
# NOTE: this prerequisite-only rule MUST NOT be the first rule in the file —
# the first rule is the default goal, and having it here once turned plain
# `make` into "check one .o and stop" (three deploys shipped a stale DLL).
.DEFAULT_GOAL := all
HDRS := $(wildcard src/*.h src/cpu/*.h src/sys/*.h)
$(OBJECTS): $(HDRS)

# ---- platform selection ----
ifeq ($(OS),Windows_NT)
   platform ?= win
else
   platform ?= unix
endif

ifeq ($(platform),win)
   TARGET  := $(TARGET_NAME)_libretro.dll
   LDFLAGS += -static-libgcc -Wl,--no-undefined -s
   EXE     := .exe
else ifeq ($(platform),unix)
   TARGET  := $(TARGET_NAME)_libretro.so
   CFLAGS  += -fPIC
   LDFLAGS += -fPIC -Wl,--no-undefined -s
   EXE     :=
else
   $(error Unknown platform '$(platform)' - supported: win, unix)
endif

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# --- CPU test tools (native, not part of the core) ---
SPIKE := cpu_spike$(EXE)
RUN   := cpu_run$(EXE)
TOOLCFLAGS := -O2 -Wall -Wextra -std=c99 -Isrc

spike: $(SPIKE)
$(SPIKE): tools/cpu_spike.c src/cpu/sm8521_disasm.c
	$(CC) $(TOOLCFLAGS) -o $@ $^

cpurun: $(RUN)
$(RUN): tools/cpu_run.c src/cpu/sm8521.c src/cpu/sm8521_disasm.c
	$(CC) $(TOOLCFLAGS) -o $@ $^

BOOT := boot_run$(EXE)
bootrun: $(BOOT)
$(BOOT): tools/boot_run.c src/cpu/sm8521.c src/cpu/sm8521_disasm.c src/sys/gcbus.c
	$(CC) $(TOOLCFLAGS) -o $@ $^

REND := render_test$(EXE)
render: $(REND)
$(REND): tools/render_test.c src/cpu/sm8521.c src/sys/gcbus.c src/sys/ppu.c
	$(CC) $(TOOLCFLAGS) -o $@ $^

FRAME := frame_run$(EXE)
framerun: $(FRAME)
$(FRAME): tools/frame_run.c src/cpu/sm8521.c src/cpu/sm8521_disasm.c src/sys/gcbus.c src/sys/ppu.c src/sys/sound.c src/sys/gcsystem.c
	$(CC) $(TOOLCFLAGS) -o $@ $^

HOST := libretro_host$(EXE)
host: $(HOST)
$(HOST): tools/libretro_host.c
	$(CC) $(TOOLCFLAGS) -o $@ $^

CYCT := cycle_test$(EXE)
cycletest: $(CYCT)
	./$(CYCT)
$(CYCT): tools/cycle_test.c src/cpu/sm8521.c
	$(CC) $(TOOLCFLAGS) -o $@ $^

ACAP := audio_capture$(EXE)
audiocap: $(ACAP)
$(ACAP): tools/audio_capture.c src/cpu/sm8521.c src/sys/gcbus.c src/sys/ppu.c src/sys/sound.c src/sys/gcsystem.c
	$(CC) $(TOOLCFLAGS) -o $@ $^

clean:
	$(RM) $(OBJECTS) $(TARGET) $(SPIKE) $(RUN) $(BOOT) $(REND) $(FRAME) $(CYCT) $(ACAP)

.PHONY: all clean spike cpurun bootrun render framerun cycletest audiocap
