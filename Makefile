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
  # RUNPATH (new dtags) $ORIGIN/lib then $ORIGIN — one layout for the
  # dev tree (build/dsvp + build/lib), the portable bundle (dsvp +
  # lib/) and the .deb (/usr/lib/dsvp/dsvp + lib/). The old rpath
  # pointed INTO the shadercross CI artifact, whose lib/ carries its
  # own libSDL3.so.0 (0.5.0) under the same soname as the real SDL —
  # with LD_LIBRARY_PATH unset the loader took that copy (review C1).
  # build/lib is staged at link time WITHOUT libSDL3.so*.
  SC_LDFLAGS = -L$(SC_ROOT)/lib -lSDL3_shadercross -Wl,--enable-new-dtags -Wl,-rpath,'$$ORIGIN/lib:$$ORIGIN'
endif

# Stamp the build with its commit so a log can never again be ambiguous about
# which tree produced it — a wrong-branch binary once cost a day of debugging a
# fix that was never in the binary being tested. "unknown" outside a git tree.
# Only THIS tree's .git counts (review M15): git walks upward from the cwd, so
# a source tarball unpacked under a git-managed $HOME used to be stamped with
# the parent repo's SHA. A worktree's .git file matches the wildcard too.
ifeq ($(wildcard .git),)
  GIT_COMMIT := unknown
  GIT_DIRTY  :=
else
  GIT_COMMIT := $(shell git rev-parse --short HEAD 2>/dev/null || echo unknown)
  # diff-index vs HEAD: plain `git diff --quiet` ignores STAGED changes,
  # stamping a staged-but-uncommitted tree as clean — the exact ambiguity
  # this stamp exists to kill.
  GIT_DIRTY  := $(shell git diff-index --quiet HEAD -- 2>/dev/null || echo +dirty)
endif
# Build mode rides in the stamp so a packager can refuse a debug binary
# and so objects rebuild when the mode changes (review M9, m-S5-b).
BUILD_MODE := $(if $(filter debug,$(MAKECMDGOALS)),debug,$(if $(filter profile,$(MAKECMDGOALS)),profile,release))
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

debug: CFLAGS += -g -DDSVP_DEBUG
debug: $(TARGET)

# Section timing (PROF: lines every 10s + spike logs).
profile: CFLAGS += -DDSVP_PROFILE
profile: $(TARGET)

# The commit stamp and the build mode are baked into EVERY object
# (DSVP_GIT_COMMIT is in BASE_CFLAGS and dsvp.h makes it visible to all
# translation units — player.c prints it on the debug panel), so every
# object depends on the stamp file, not just main.o: an incremental
# build after a commit that touched only audio.c used to leave player.o
# with the OLD stamp and the panel disagreeing with the banner (review
# M10). The stamp file changes exactly when commit/dirty/mode change,
# so the recompile is paid exactly when it must be — and `make debug`
# after `make` now rebuilds instead of linking a non-debug binary
# (m-S5-b). NOTE: rules must stay BELOW `all:` — a rule above it
# becomes make's default goal (deck field case: bare `make` built
# FORCE, i.e. nothing).
GITSTAMP = $(OBJDIR)/.gitstamp
STAMP_TEXT = $(GIT_COMMIT)$(GIT_DIRTY) $(BUILD_MODE)
.PHONY: FORCE
FORCE:
$(GITSTAMP): FORCE | $(OBJDIR)
	@echo '$(STAMP_TEXT)' | cmp -s - $@ 2>/dev/null || echo '$(STAMP_TEXT)' > $@
$(OBJS): $(GITSTAMP)

$(OBJDIR):
	mkdir -p $(OBJDIR)

# Objects persist between builds — deleting them after every link forced a
# full recompile of all seven translation units (including 4.3k-line player.c)
# on every make. `make clean` still removes build/ entirely.
$(TARGET): $(OBJS) $(RC_OBJ)
	$(CC) -o $@ $^ $(LDFLAGS)
	@echo '$(STAMP_TEXT)' > $(BUILDDIR)/dsvp.stamp
	@echo "stamp: $(STAMP_TEXT)"
ifeq ($(OS),Windows_NT)
	cp $(SDL3_BIN)/SDL3.dll $(BUILDDIR)/
	cp $(SDL3_BIN)/SDL3_ttf.dll $(BUILDDIR)/
	cp $(SC_ROOT)/bin/SDL3_shadercross.dll $(BUILDDIR)/
	cp $(SC_ROOT)/bin/dxcompiler.dll $(BUILDDIR)/
	cp $(SC_ROOT)/bin/dxil.dll $(BUILDDIR)/
# Plain cp, not cp -u (review m-S5-a): after a pacman downgrade the
# installed DLL is OLDER than the one in build/ and -u kept the stale
# one next to an exe linked against the new import lib.
# SDL3_shadercross.dll links against spirv-cross; without it the loader
# fails with "SDL3_shadercross.dll: cannot open shared object file",
# naming the DLL that IS present rather than the one that is missing.
	cp $(SC_ROOT)/bin/libspirv-cross-c-shared.dll $(BUILDDIR)/
else
# Stage the shadercross runtime libs beside the binary ($ORIGIN/lib),
# EXCLUDING the CI artifact's own libSDL3.so* (review C1). Real files
# only; the soname symlinks are recreated.
	@mkdir -p $(BUILDDIR)/lib
	@for so in $(SC_ROOT)/lib/*.so*; do \
	    b=$$(basename "$$so"); \
	    case "$$b" in libSDL3.so*) continue ;; esac; \
	    if [ -L "$$so" ]; then ln -sfn "$$(readlink "$$so")" "$(BUILDDIR)/lib/$$b"; \
	    elif [ -f "$$so" ]; then cp "$$so" "$(BUILDDIR)/lib/$$b"; fi; \
	done
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
