# DSVP — Dead Simple Video Player
# Makefile for SDL3 build (v0.1.3-beta)

CC      = gcc
SRCDIR  = src
BUILDDIR = build

CFLAGS  = -Wall -Wextra -O2 $(shell pkg-config --cflags sdl3 SDL3_ttf libavformat libavcodec libavutil libswscale libswresample)
LDFLAGS = $(shell pkg-config --libs sdl3 SDL3_ttf libavformat libavcodec libavutil libswscale libswresample) -lm

# If pkg-config doesn't find SDL3_ttf, try sdl3-ttf
ifeq ($(shell pkg-config --exists SDL3_ttf 2>/dev/null && echo yes),)
  CFLAGS  = -Wall -Wextra -O2 $(shell pkg-config --cflags sdl3 sdl3-ttf libavformat libavcodec libavutil libswscale libswresample 2>/dev/null)
  LDFLAGS = $(shell pkg-config --libs sdl3 sdl3-ttf libavformat libavcodec libavutil libswscale libswresample 2>/dev/null) -lm
endif

SRCS    = main.c player.c audio.c subtitle.c log.c
OBJS    = $(SRCS:%.c=$(BUILDDIR)/%.o)
TARGET  = $(BUILDDIR)/dsvp

.PHONY: all clean debug

all: $(BUILDDIR) $(TARGET)

debug: CFLAGS += -g -DDSVP_DEBUG
debug: $(BUILDDIR) $(TARGET)

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)
	rm -f $(OBJS)

$(BUILDDIR)/%.o: $(SRCDIR)/%.c $(SRCDIR)/dsvp.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf $(BUILDDIR)
