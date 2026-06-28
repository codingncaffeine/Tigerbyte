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

SOURCES := src/tigerbyte_libretro.c
OBJECTS := $(SOURCES:.c=.o)

# ---- platform selection ----
ifeq ($(OS),Windows_NT)
   platform ?= win
else
   platform ?= unix
endif

ifeq ($(platform),win)
   TARGET  := $(TARGET_NAME)_libretro.dll
   LDFLAGS += -static-libgcc -Wl,--no-undefined
   EXE     := .exe
else ifeq ($(platform),unix)
   TARGET  := $(TARGET_NAME)_libretro.so
   CFLAGS  += -fPIC
   LDFLAGS += -fPIC -Wl,--no-undefined
   EXE     :=
else
   $(error Unknown platform '$(platform)' - supported: win, unix)
endif

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# --- CPU decode spike (native test tool, not part of the core) ---
SPIKE := cpu_spike$(EXE)
spike: $(SPIKE)
$(SPIKE): tools/cpu_spike.c src/cpu/sm8521_disasm.c
	$(CC) -O2 -Wall -Wextra -std=c99 -Isrc -o $@ $^

clean:
	$(RM) $(OBJECTS) $(TARGET) $(SPIKE)

.PHONY: all clean spike
