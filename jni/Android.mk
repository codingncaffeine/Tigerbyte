# Android (NDK) build for the Tigerbyte libretro core.
#   cd jni && ndk-build
# Produces libretro.so per ABI; rename to tigerbyte_libretro_android.so for RetroArch.
LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE     := retro
CORE_DIR         := $(LOCAL_PATH)/../src
LOCAL_SRC_FILES  := $(CORE_DIR)/tigerbyte_libretro.c \
                    $(CORE_DIR)/cpu/sm8521.c \
                    $(CORE_DIR)/sys/gcbus.c \
                    $(CORE_DIR)/sys/ppu.c \
                    $(CORE_DIR)/sys/sound.c \
                    $(CORE_DIR)/sys/gcsystem.c
LOCAL_C_INCLUDES := $(CORE_DIR)
LOCAL_CFLAGS     := -O2 -std=c99 -DANDROID
include $(BUILD_SHARED_LIBRARY)
