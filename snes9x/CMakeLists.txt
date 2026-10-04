cmake_minimum_required(VERSION 3.5)
set(COMPONENTS "main retro-go snes9x")
set(RG_ENABLE_NETWORKING 0)
include(../base.cmake)
project(snes9x)
# A longer audio ring than the default 22.5 ms: the frame time varies with frameskip, and main/main_snes.c writes
# the in-game saves to the SD card between frames. 10 x 180 frames, 56 ms at 32040 Hz, 7 KB of DMA memory
idf_build_set_property(COMPILE_DEFINITIONS "-DRG_AUDIO_DMA_BUFFER_COUNT=10" APPEND)
