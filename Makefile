# DSVP Makefile
# Targets: Windows (MinGW), Linux, macOS

CC       = gcc
CFLAGS   = -Wall -Wextra -O2 -std=c11
CFLAGS  += -D_REENTRANT

SRCS     = src/main.c src/player.c src/audio.c src/log.c
TARGET   = build/dsvp

# --- Platform detection ---

ifeq ($(OS),Windows_NT)
    # Windows / MinGW
    TARGET       := build/dsvp.exe
    CFLAGS       += -I./deps/ffmpeg/include -I./deps/SDL2/include
    LDFLAGS       = -L./deps/ffmpeg/lib -L./deps/SDL2/lib
    LIBS          = -lmingw32 -lSDL2main -lSDL2
    LIBS         += -lavformat -lavcodec -lswscale -lswresample -lavutil
    LIBS         += -lm -lpthread
    # Win32 API for native file dialog
    LIBS         += -lole32 -lcomdlg32 -luuid
    # Hide console window for release (comment out for debug)
    LDFLAGS      += -mwindows
else
    UNAME_S := $(shell uname -s)
    ifeq ($(UNAME_S),Darwin)
        # macOS
        CFLAGS   += $(shell pkg-config --cflags libavformat libavcodec libswscale libswresample libavutil sdl2)
        LIBS      = $(shell pkg-config --libs libavformat libavcodec libswscale libswresample libavutil sdl2)
    else
        # Linux
        CFLAGS   += $(shell pkg-config --cflags libavformat libavcodec libswscale libswresample libavutil sdl2)
        LIBS      = $(shell pkg-config --libs libavformat libavcodec libswscale libswresample libavutil sdl2)
        LIBS     += -lm -lpthread
    endif
endif

# --- Build rules ---

all: dirs $(TARGET)

dirs:
ifeq ($(OS),Windows_NT)
	@if not exist build mkdir build
else
	@mkdir -p build
endif

$(TARGET): $(SRCS) src/dsvp.h src/log.c
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS) $(LIBS)

debug: CFLAGS += -g -DDSVP_DEBUG -O0
debug: LDFLAGS := $(filter-out -mwindows,$(LDFLAGS))
debug: all

clean:
ifeq ($(OS),Windows_NT)
	@if exist build\dsvp.exe del build\dsvp.exe
else
	rm -f $(TARGET)
endif

.PHONY: all dirs clean debug
