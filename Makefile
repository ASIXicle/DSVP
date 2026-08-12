# DSVP — Dead Simple Video Player
# Makefile for SDL_GPU build (version comes from src/dsvp.h — do not hardcode here)

CC      = gcc
SRCDIR  = src
BUILDDIR = build
# Objects and dependency maps live out of sight — build/ itself holds
# only what runs (the binary, its log, the shader cache).
OBJDIR   = $(BUILDDIR)/obj

# ── Base flags (SDL3, FFmpeg) ──
BASE_CFLAGS  = -Wall -Wextra -O2 $(shell pkg-config --cflags sdl3 SDL3_ttf libavformat libavcodec libavfilter libavutil libswscale libswresample)
BASE_LDFLAGS = $(shell pkg-config --libs sdl3 SDL3_ttf libavformat libavcodec libavfilter libavutil libswscale libswresample) -lm -lz

# If pkg-config doesn't find SDL3_ttf, try sdl3-ttf
ifeq ($(shell pkg-config --exists SDL3_ttf 2>/dev/null && echo yes),)
  ifeq ($(shell pkg-config --exists sdl3-ttf 2>/dev/null && echo yes),)
    $(error pkg-config found neither 'SDL3_ttf' nor 'sdl3-ttf' — check PKG_CONFIG_PATH (see SETUP.md); without this the flags expand empty and the build fails with a cryptic missing-header error)
  endif
  BASE_CFLAGS  = -Wall -Wextra -O2 $(shell pkg-config --cflags sdl3 sdl3-ttf libavformat libavcodec libavfilter libavutil libswscale libswresample 2>/dev/null)
  BASE_LDFLAGS = $(shell pkg-config --libs sdl3 sdl3-ttf libavformat libavcodec libavfilter libavutil libswscale libswresample 2>/dev/null) -lm -lz
endif

# FFmpeg presence check — without it, a missing .pc set expands the
# flags EMPTY (the 2>/dev/null above) and the user gets a cryptic
# missing-header error instead of this message. System SDL .pc files
# often exist while FFmpeg's don't (field case: clean-shell build).
ifeq ($(shell pkg-config --exists libavformat libavcodec libavfilter 2>/dev/null && echo yes),)
  $(error pkg-config cannot find FFmpeg dev libraries — set PKG_CONFIG_PATH to your prefix (see SETUP.md))
endif

# ── Windows: explicit link for Unicode Win32 APIs ──
ifeq ($(OS),Windows_NT)
  BASE_LDFLAGS += -lshell32 -lcomdlg32
endif

# ── SDL3_shadercross (bundled on Windows, pkg-config on Linux) ──
ifeq ($(OS),Windows_NT)
  SC_ROOT    = deps/SDL3_shadercross-3.0.0-windows-mingw-x64
  SC_CFLAGS  = -I$(SC_ROOT)/include
  SC_LDFLAGS = -L$(SC_ROOT)/lib -lSDL3_shadercross
else
  SC_ROOT    = shadercross/SDL3_shadercross-3.0.0-linux-x64
  SC_CFLAGS  = -I$(SC_ROOT)/include
  SC_LDFLAGS = -L$(SC_ROOT)/lib -lSDL3_shadercross -Wl,-rpath,'$$ORIGIN/../shadercross/SDL3_shadercross-3.0.0-linux-x64/lib'
endif

# Stamp the build with its commit so a log can never again be ambiguous about
# which tree produced it — a wrong-branch binary once cost a day of debugging a
# fix that was never in the binary being tested. "unknown" outside a git tree.
GIT_COMMIT := $(shell git rev-parse --short HEAD 2>/dev/null || echo unknown)
# diff-index vs HEAD: plain `git diff --quiet` ignores STAGED changes,
# stamping a staged-but-uncommitted tree as clean — the exact ambiguity
# this stamp exists to kill.
GIT_DIRTY  := $(shell git diff-index --quiet HEAD -- 2>/dev/null || echo +dirty)
BASE_CFLAGS += -DDSVP_GIT_COMMIT=\"$(GIT_COMMIT)$(GIT_DIRTY)\" -MMD -MP

CFLAGS  = $(BASE_CFLAGS) $(SC_CFLAGS)
LDFLAGS = $(BASE_LDFLAGS) $(SC_LDFLAGS)

SRCS    = main.c player.c audio.c bitstream.c subtitle.c overlay.c log.c
OBJS    = $(SRCS:%.c=$(OBJDIR)/%.o)
DEPS    = $(OBJS:%.o=%.d)

# Windows: append .exe, locate SDL3 DLLs via pkg-config, compile .rc for icon
ifeq ($(OS),Windows_NT)
  TARGET   = $(BUILDDIR)/dsvp.exe
  SDL3_BIN = $(shell pkg-config --variable=prefix sdl3)/bin
  RC_OBJ   = $(OBJDIR)/dsvp_res.o
else
  TARGET   = $(BUILDDIR)/dsvp
  RC_OBJ   =
endif

.PHONY: all clean debug profile

all: $(TARGET)

# Same caveat as profile: no flag tracking on objects — an incremental
# `make debug` after `make` finds everything up-to-date and links a
# NON-debug binary. Always `make clean && make debug`.
debug: CFLAGS += -g -DDSVP_DEBUG
debug: $(TARGET)

# Section timing (PROF: lines every 10s + spike logs). No flag
# tracking on objects — always `make clean && make profile`, and
# `make clean` again to return to a normal build.
profile: CFLAGS += -DDSVP_PROFILE
profile: $(TARGET)

# The stamp is baked into main.o at compile time, so an incremental
# build that does not touch main.c ships a STALE stamp (deck field
# case: a binary logging a two-commits-old build id). The stamp file's
# content changes exactly when the commit/dirty state does, and main.o
# depends on it. NOTE: rules must stay BELOW `all:` — a rule above it
# becomes make's default goal (deck field case: bare `make` built
# FORCE, i.e. nothing).
GITSTAMP = $(OBJDIR)/.gitstamp
.PHONY: FORCE
FORCE:
$(GITSTAMP): FORCE | $(OBJDIR)
	@echo '$(GIT_COMMIT)$(GIT_DIRTY)' | cmp -s - $@ 2>/dev/null || echo '$(GIT_COMMIT)$(GIT_DIRTY)' > $@
$(OBJDIR)/main.o: $(GITSTAMP)

$(OBJDIR):
	mkdir -p $(OBJDIR)

# Objects persist between builds — deleting them after every link forced a
# full recompile of all seven translation units (including 4.3k-line player.c)
# on every make. `make clean` still removes build/ entirely.
$(TARGET): $(OBJS) $(RC_OBJ)
	$(CC) -o $@ $^ $(LDFLAGS)
ifeq ($(OS),Windows_NT)
	cp -u $(SDL3_BIN)/SDL3.dll $(BUILDDIR)/
	cp -u $(SDL3_BIN)/SDL3_ttf.dll $(BUILDDIR)/
	cp -u $(SC_ROOT)/bin/SDL3_shadercross.dll $(BUILDDIR)/
	cp -u $(SC_ROOT)/bin/dxcompiler.dll $(BUILDDIR)/
	cp -u $(SC_ROOT)/bin/dxil.dll $(BUILDDIR)/
# SDL3_shadercross.dll links against spirv-cross; without it the loader
# fails with "SDL3_shadercross.dll: cannot open shared object file",
# naming the DLL that IS present rather than the one that is missing.
	cp -u $(SC_ROOT)/bin/libspirv-cross-c-shared.dll $(BUILDDIR)/
endif

# Windows resource file (application icon for taskbar/explorer)
$(OBJDIR)/dsvp_res.o: dsvp.rc src/dsvp.ico | $(OBJDIR)
	windres $< -o $@

# -MMD -MP (in CFLAGS) emits a .d per object listing every header it actually
# included, so header edits rebuild exactly the objects that use them.
$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

-include $(DEPS)

clean:
	rm -rf $(BUILDDIR)
