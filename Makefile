#---------------------------------------------------------------------------------
# Modern Combat 3 -- Nintendo Switch wrapper (32-bit / AArch32, android32)
#
# Ships NO game code and NO game assets: the player's own APK (1.1.7g) and OBB
# files are read at run time. Build with ./build.sh (the toolchain container).
#---------------------------------------------------------------------------------
TARGET               := mc3_nx
PORT_NPDM_PROGRAM_ID := 0x01000000000010C3
PORT_NPDM_VERSION    := 1.0.1
# The engine ran on a 9 MB stack in the PS Vita port; here it runs on main.
PORT_NPDM_MAIN_STACK := 0xA00000
# The engine reads its OBB zips with many small freads.
PORT_CFLAGS += -DRT_STDIO_READ_BUF=65536

# Briefing videos: with FFmpeg (tools/build_ffmpeg.sh) and Mesa in
# portlibs32/; without them the game is told each video finished at once.
ifneq ($(and $(wildcard $(CURDIR)/portlibs32/lib/libavcodec.a),$(wildcard $(CURDIR)/portlibs32/lib/libEGL.a)),)
MC3_VIDEO := 1
PORT_CFLAGS += -DMC3_VIDEO=1
PORT_LIBS += -L$(CURDIR)/portlibs32/lib -lavformat -lavcodec -lavutil
else
MC3_VIDEO := 0
endif
PORT_STAMP := $(PORT_STAMP)-video$(MC3_VIDEO)

include runtime/runtime.mk

# FFmpeg assumes int-sized enums; devkitARM defaults to short ones. The one
# file that touches FFmpeg's structs is built like FFmpeg (mc3_video_ff.h).
$(BUILD)/mc3_video_ff.o: CFLAGS += -fno-short-enums
