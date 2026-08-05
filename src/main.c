/*
 * DSVP — Dead Simple Video Player
 * main.c — Entry point, SDL initialization, event loop
 *
 * This is the application's main loop. It:
 *   1. Initializes SDL (video, audio, events)
 *   2. Creates the window and GPU device (SDL_GPU)
 *   3. Compiles HLSL shaders via shadercross
 *   4. Processes keyboard/mouse events
 *   5. Drives video decode and rendering via GPU
 *
 * Phase 2 (v0.1.4-beta): Full GPU rendering with overlay system.
 * Video frames rendered via custom HLSL shaders (SDL_GPU). Overlays
 * (debug, info, seek bar, subtitles, OSD) composited as RGBA texture
 * with alpha blending over the video quad.
 */

#include "dsvp.h"
#include "dsvp_icon.h"

/* Baked in by the Makefile (git short SHA, +dirty when the tree is modified).
 * "unknown" only when built outside a git checkout. */
#ifndef DSVP_GIT_COMMIT
#define DSVP_GIT_COMMIT "unknown"
#endif
#ifndef _WIN32
  #include <dirent.h>
#endif

/* Platform-specific file dialog */
#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <commdlg.h>
  #include <shellapi.h>   /* CommandLineToArgvW */

/* Convert UTF-16 wide string to UTF-8.  Caller must free() the result. */
static char *win_wide_to_utf8(const wchar_t *wstr) {
    if (!wstr) return NULL;
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, NULL, 0, NULL, NULL);
    if (len <= 0) return NULL;
    char *out = malloc(len);
    if (!out) return NULL;
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, out, len, NULL, NULL);
    return out;
}

/* Convert UTF-8 string to UTF-16 wide.  Caller must free() the result. */
static wchar_t *win_utf8_to_wide(const char *str) {
    if (!str) return NULL;
    int len = MultiByteToWideChar(CP_UTF8, 0, str, -1, NULL, 0);
    if (len <= 0) return NULL;
    wchar_t *out = malloc(len * sizeof(wchar_t));
    if (!out) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, str, -1, out, len);
    return out;
}
#endif

/* ═══════════════════════════════════════════════════════════════════
 * File Open Dialog
 * ═══════════════════════════════════════════════════════════════════ */

/* Returns 1 if a file was selected (path written to `out`), 0 if cancelled. */
static int open_file_dialog(char *out, int out_size) {
#ifdef _WIN32
    /* Native Win32 file dialog — wide (Unicode) version */
    OPENFILENAMEW ofn;
    wchar_t file[1024] = {0};

    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize  = sizeof(ofn);
    ofn.hwndOwner    = NULL;
    ofn.lpstrFile    = file;
    ofn.nMaxFile     = sizeof(file) / sizeof(file[0]);
    ofn.lpstrFilter  = L"Video Files\0"
                       L"*.mkv;*.mp4;*.avi;*.mov;*.wmv;*.flv;*.webm;*.m4v;*.ts;*.mpg;*.mpeg\0"
                       L"Audio Files\0"
                       L"*.mp3;*.flac;*.wav;*.aac;*.ogg;*.opus;*.m4a;*.wma\0"
                       L"All Files\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags        = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameW(&ofn)) {
        char *utf8 = win_wide_to_utf8(file);
        if (utf8) {
            snprintf(out, out_size, "%s", utf8);
            free(utf8);
            return 1;
        }
    }
    return 0;

#else
    /* Linux/macOS: try multiple dialog backends */
    FILE *fp = NULL;

    #ifdef __APPLE__
    fp = popen("osascript -e 'POSIX path of (choose file of type {\"public.movie\", \"public.audio\"})'", "r");
    #else
    /* Try zenity, then kdialog, then yad */
    const char *commands[] = {
        "zenity --file-selection --title='Open Media File' "
            "--file-filter='Media files|*.mkv *.mp4 *.avi *.mov *.wmv *.flv *.webm *.m4v *.ts *.mpg *.mpeg *.mp3 *.flac *.wav *.aac *.ogg *.opus *.m4a *.wma' "
            "--file-filter='All files|*' 2>/dev/null",
        "kdialog --getopenfilename . "
            "'Media files (*.mkv *.mp4 *.avi *.mov *.wmv *.flv *.webm *.m4v *.ts *.mpg *.mpeg *.mp3 *.flac *.wav *.aac *.ogg *.opus *.m4a *.wma)' 2>/dev/null",
        "yad --file-selection --title='Open Media File' 2>/dev/null",
        NULL
    };
    const char *names[] = { "zenity", "kdialog", "yad" };

    for (int i = 0; commands[i]; i++) {
        /* Check if the tool exists before trying it */
        char which_cmd[64];
        snprintf(which_cmd, sizeof(which_cmd), "which %s >/dev/null 2>&1", names[i]);
        if (system(which_cmd) == 0) {
            log_msg("File dialog: using %s", names[i]);
            fp = popen(commands[i], "r");
            break;
        }
    }

    if (!fp) {
        log_msg("ERROR: No file dialog available. Install zenity, kdialog, or yad.");
        log_msg("  Debian/Ubuntu: sudo apt install zenity");
        log_msg("  Fedora: sudo dnf install zenity");
        log_msg("  Tip: you can also pass a file path on the command line: ./dsvp video.mp4");
        return 0;
    }
    #endif

    if (!fp) return 0;

    if (fgets(out, out_size, fp)) {
        /* Remove trailing newline */
        size_t len = strlen(out);
        if (len > 0 && out[len - 1] == '\n') out[len - 1] = '\0';
        pclose(fp);
        return (strlen(out) > 0) ? 1 : 0;
    }
    pclose(fp);
    return 0;
#endif
}


/* ═══════════════════════════════════════════════════════════════════
 * Folder Playlist — prev/next file navigation
 * ═══════════════════════════════════════════════════════════════════
 *
 * Scans the parent directory of the current file for playable media,
 * sorts alphabetically, and allows navigating to adjacent entries.
 */

static const char *video_extensions[] = {
    ".mkv", ".mp4", ".avi", ".mov", ".wmv", ".flv", ".webm", ".m4v",
    ".ts", ".m2ts", ".mpg", ".mpeg", ".3gp",
    ".mp3", ".flac", ".wav", ".aac", ".ogg", ".opus", ".m4a", ".wma",
    NULL
};

/* HDR midtone gain index — file-scope so it can be reset on file open.
 * Index 3 = 1.3f, matching the default gpu_uniforms.hdr_midtone_gain. */
static int s_gain_idx = 3;

/* Reset gain state AND the uniform together. Resetting only the index
 * left the previous file's gain in the shader while the state machine
 * believed 1.3 — the first G press then jumped to 1.35, skipping the
 * advertised default. (hdr_target_nits deliberately persists across
 * files, WITH its index — that pairing is the model to follow.) */
static void reset_gain(PlayerState *ps) {
    s_gain_idx = 3;
    ps->gpu_uniforms.hdr_midtone_gain = 1.3f;
    ps->cache_valid = 0;
}

static int is_media_file(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    for (int i = 0; video_extensions[i]; i++) {
#ifdef _WIN32
        if (_stricmp(dot, video_extensions[i]) == 0) return 1;
#else
        if (strcasecmp(dot, video_extensions[i]) == 0) return 1;
#endif
    }
    return 0;
}

/* Natural-order, case-insensitive compare: digit runs compare as
 * numbers, so "E2" sorts before "E10" — byte-wise case-folded compare
 * played episodes out of story order in any season folder with
 * unpadded numbers. ASCII-only case folding (matches the old
 * strcasecmp behavior for ASCII; non-ASCII bytes compare as unsigned).
 * Leading zeros: shorter digit run wins ties so the ordering stays a
 * strict total order for qsort ("01" < "1" deterministically, "2" <
 * "010" numerically). */
static int natural_casecmp(const char *a, const char *b) {
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= '0' && ca <= '9' && cb >= '0' && cb <= '9') {
            /* Skip leading zeros, remember how many for the tie-break */
            const char *pa = a, *pb = b;
            while (*pa == '0') pa++;
            while (*pb == '0') pb++;
            const char *da = pa, *db = pb;
            while (*da >= '0' && *da <= '9') da++;
            while (*db >= '0' && *db <= '9') db++;
            ptrdiff_t la = da - pa, lb = db - pb;
            if (la != lb) return (la < lb) ? -1 : 1;   /* more digits = larger */
            for (; pa < da; pa++, pb++)
                if (*pa != *pb) return (*pa < *pb) ? -1 : 1;
            /* Equal values — fewer leading zeros sorts first */
            ptrdiff_t za = pa - a - la, zb = pb - b - lb;
            if (za != zb) return (za < zb) ? -1 : 1;
            a = da; b = db;
            continue;
        }
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return (ca < cb) ? -1 : 1;
        a++; b++;
    }
    if (*a) return 1;
    if (*b) return -1;
    return 0;
}

static int cmp_strings(const void *a, const void *b) {
    const char *sa = *(const char **)a;
    const char *sb = *(const char **)b;
    return natural_casecmp(sa, sb);
}

static void playlist_free(PlayerState *ps) {
    if (ps->playlist_files) {
        for (int i = 0; i < ps->playlist_count; i++)
            free(ps->playlist_files[i]);
        free(ps->playlist_files);
        ps->playlist_files = NULL;
    }
    ps->playlist_count = 0;
    ps->playlist_index = -1;
}

/* Scan the directory containing `filepath` for playable media files.
 * Populates ps->playlist_files (sorted), playlist_count, playlist_index. */
static void playlist_scan(PlayerState *ps) {
    playlist_free(ps);
    if (!ps->filepath[0]) return;

    /* Extract directory and filename from filepath */
    char dir[1024], base[1024];
    strncpy(dir, ps->filepath, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';

    /* Find last separator */
    char *sep = strrchr(dir, '/');
#ifdef _WIN32
    char *sep2 = strrchr(dir, '\\');
    if (sep2 && (!sep || sep2 > sep)) sep = sep2;
#endif
    /* prefix is prepended to scanned names to build entries comparable
     * to ps->filepath. For a bare relative name ("dsvp movie.mkv" run
     * from inside the folder) the old code set dir="." with no
     * separator: entries came out as ".movie.mkv", the index lookup
     * against "movie.mkv" failed, B/N went dead, and on Windows the
     * find pattern ".*" matched only dot-entries — empty playlist. */
    char prefix[1024];
    if (sep) {
        strncpy(base, sep + 1, sizeof(base) - 1);
        base[sizeof(base) - 1] = '\0';
        *(sep + 1) = '\0';  /* dir now ends with separator */
        snprintf(prefix, sizeof(prefix), "%s", dir);
    } else {
        strncpy(base, dir, sizeof(base) - 1);
        base[sizeof(base) - 1] = '\0';
        strcpy(dir, "./");  /* scan location */
        prefix[0] = '\0';   /* filepath was bare — keep entries bare */
    }

    /* Scan directory */
    int capacity = 64;
    char **files = malloc(capacity * sizeof(char *));
    if (!files) return;
    int count = 0;

#ifdef _WIN32
    /* Windows: FindFirstFileW/FindNextFileW for Unicode filenames */
    {
        char pattern[2048];
        snprintf(pattern, sizeof(pattern), "%s*", dir);
        wchar_t *wpattern = win_utf8_to_wide(pattern);
        if (!wpattern) { free(files); return; }

        WIN32_FIND_DATAW fd;
        HANDLE hFind = FindFirstFileW(wpattern, &fd);
        free(wpattern);
        if (hFind == INVALID_HANDLE_VALUE) {
            log_msg("playlist_scan: cannot open directory: %s", dir);
            free(files);
            return;
        }

        do {
            /* Skip . and .. only — the dotfile-hidden convention is a
             * Unix thing; ".movie.mkv" is a legitimate name on Windows. */
            if (fd.cFileName[0] == L'.' &&
                (fd.cFileName[1] == L'\0' ||
                 (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0')))
                continue;
            /* A folder named "Backups.old.mp4" is not a media file. */
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            char *name = win_wide_to_utf8(fd.cFileName);
            if (!name) continue;
            if (!is_media_file(name)) { free(name); continue; }

            char fullpath[2048];
            snprintf(fullpath, sizeof(fullpath), "%s%s", prefix, name);
            free(name);

            if (count >= capacity) {
                capacity *= 2;
                char **tmp = realloc(files, capacity * sizeof(char *));
                if (!tmp) break;
                files = tmp;
            }
            files[count] = strdup(fullpath);
            if (!files[count]) break;
            count++;
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }
#else
    /* POSIX: opendir/readdir (UTF-8 native on Linux/macOS) */
    {
        DIR *d = opendir(dir);
        if (!d) {
            log_msg("playlist_scan: cannot open directory: %s", dir);
            free(files);
            return;
        }

        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            if (!is_media_file(ent->d_name)) continue;
            /* A directory named "Backups.old.mp4" is not a media file.
             * d_type is DT_UNKNOWN on some filesystems — only skip when
             * we positively know it's a directory (a bad playlist entry
             * just fails to open, recoverably). */
            if (ent->d_type == DT_DIR) continue;

            char fullpath[2048];
            snprintf(fullpath, sizeof(fullpath), "%s%s", prefix, ent->d_name);

            if (count >= capacity) {
                capacity *= 2;
                char **tmp = realloc(files, capacity * sizeof(char *));
                if (!tmp) break;
                files = tmp;
            }
            files[count] = strdup(fullpath);
            if (!files[count]) break;
            count++;
        }
        closedir(d);
    }
#endif

    if (count == 0) {
        free(files);
        return;
    }

    /* Sort alphabetically */
    qsort(files, count, sizeof(char *), cmp_strings);

    ps->playlist_files = files;
    ps->playlist_count = count;

    /* Find current file's index */
    ps->playlist_index = -1;
    for (int i = 0; i < count; i++) {
        /* Compare against full filepath */
#ifdef _WIN32
        if (_stricmp(files[i], ps->filepath) == 0) {
#else
        if (strcmp(files[i], ps->filepath) == 0) {
#endif
            ps->playlist_index = i;
            break;
        }
    }

    log_msg("playlist_scan: %d files in folder, current index=%d",
            count, ps->playlist_index);
}


/* ═══════════════════════════════════════════════════════════════════
 * GPU Idle Screen (no media loaded)
 * ═══════════════════════════════════════════════════════════════════
 *
 * Dark background with DSVP title, version, and hotkey reference
 * rendered via the overlay system.
 */

/* Submit a dark-background frame with the CURRENT overlay buffer.
 * Used by the idle screen and by audio-only playback (which has no
 * video texture to blit but still needs the seek bar / OSD / info
 * panels presented each tick). Caller is responsible for having
 * rendered the overlay (overlay_render or overlay_render_idle). */
static void gpu_submit_background(PlayerState *ps) {
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(ps->gpu_device);
    if (!cmd) return;

    /* Upload overlay texture if dirty */
    gpu_overlay_copy_cmd(cmd, ps);

    SDL_GPUTexture *swapchain_tex = NULL;
    Uint32 sc_w, sc_h;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, ps->window,
            &swapchain_tex, &sc_w, &sc_h)) {
        SDL_CancelGPUCommandBuffer(cmd);
        return;
    }
    if (!swapchain_tex) {
        SDL_CancelGPUCommandBuffer(cmd);
        return;
    }

    /* Dark background — same color as old idle screen (24, 24, 28) */
    SDL_GPUColorTargetInfo color_target;
    SDL_zero(color_target);
    color_target.texture    = swapchain_tex;
    color_target.clear_color = (SDL_FColor){ 0.094f, 0.094f, 0.110f, 1.0f };
    color_target.load_op    = SDL_GPU_LOADOP_CLEAR;
    color_target.store_op   = SDL_GPU_STOREOP_STORE;

    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &color_target, 1, NULL);
    {
        /* Overlay quad */
        gpu_overlay_draw(pass, cmd, ps, sc_w, sc_h);
    }
    SDL_EndGPURenderPass(pass);

    SDL_SubmitGPUCommandBuffer(cmd);
}

static void gpu_draw_idle(PlayerState *ps) {
    /* Update physical pixel dimensions for the idle window.
     * After player_close resets the window to 960×540, the stale
     * sc_w/sc_h from the video session would cause overlay_render_idle
     * to draw at the wrong size. */
    int phys_w, phys_h;
    SDL_GetWindowSizeInPixels(ps->window, &phys_w, &phys_h);
    ps->sc_w = phys_w;
    ps->sc_h = phys_h;

    /* Render idle screen text to overlay pixel buffer */
    overlay_render_idle(ps);

    gpu_submit_background(ps);
}


/* ═══════════════════════════════════════════════════════════════════
 * Fullscreen
 *
 * One function owns every fullscreen transition. The F key and the
 * double-click path used to be two hand-copies of the same six lines —
 * the drift-prone pattern that bit the deck build. ps->fullscreen is a
 * cached mirror that the compositor can invalidate out-of-band (WM
 * keybind, refused request), so the toggle reads the WINDOW's actual
 * state, and ENTER/LEAVE_FULLSCREEN events re-sync the mirror.
 * ═══════════════════════════════════════════════════════════════════ */

static void set_fullscreen(PlayerState *ps, SDL_Window *window, bool want_fs) {
    /* Pause audio across the transition to prevent drift */
    if (ps->playing && !ps->paused && ps->audio_stream)
        SDL_PauseAudioStreamDevice(ps->audio_stream);

    ps->fullscreen = want_fs;
    SDL_SetWindowFullscreen(window, want_fs);

    if (!want_fs && ps->playing && ps->vid_w > 0 && ps->vid_h > 0) {
        /* Resize to the current video's aspect ratio. Without this,
         * leaving fullscreen keeps whatever window shape was left
         * behind, with stale black bars. */
        const SDL_DisplayMode *dm =
            SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
        int max_w = dm ? (int)(dm->w * 0.8) : 1920;
        int max_h = dm ? (int)(dm->h * 0.8) : 1080;
        int w = ps->vid_w, h = ps->vid_h;
        if (w > max_w || h > max_h) {
            double scale = fmin((double)max_w / w, (double)max_h / h);
            w = (int)(w * scale);
            h = (int)(h * scale);
        }
        SDL_SetWindowSize(window, w, h);
    }

    if (ps->playing) {
        ps->frame_timer = get_time_sec();
        if (!ps->paused && ps->audio_stream)
            SDL_ResumeAudioStreamDevice(ps->audio_stream);
    }
    log_msg("FS: %s", want_fs ? "entered fullscreen (borderless)"
                              : "returned to windowed");
}

static void toggle_fullscreen(PlayerState *ps, SDL_Window *window) {
    bool is_fs = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
    set_fullscreen(ps, window, !is_fs);
}

/* ═══════════════════════════════════════════════════════════════════
 * Main
 * ═══════════════════════════════════════════════════════════════════ */

int main(int argc, char *argv[]) {
    /* ── Initialize logging (before anything else) ── */
    (void)argv;  /* replaced by CommandLineToArgvW for Unicode support */
    log_init();
    log_msg("Starting DSVP v" DSVP_VERSION " build " DSVP_GIT_COMMIT " (argc=%d)", argc);
    log_msg("FFmpeg %s (libavcodec %d.%d)", av_version_info(),
            LIBAVCODEC_VERSION_MAJOR, LIBAVCODEC_VERSION_MINOR);

    /* ── Get UTF-8 filepath from command line ──
     * On Windows, argv[] is in the system ANSI codepage, which corrupts
     * non-ASCII characters (accents, CJK, fullwidth punctuation).
     * Use GetCommandLineW → CommandLineToArgvW → UTF-8 conversion. */
    char *open_path = NULL;
#ifdef _WIN32
    {
        int wargc = 0;
        LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
        if (wargv && wargc > 1) {
            open_path = win_wide_to_utf8(wargv[1]);
        }
        if (wargv) LocalFree(wargv);
    }
#else
    if (argc > 1)
        open_path = strdup(argv[1]);
#endif

    /* ── Initialize SDL ── */
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        fprintf(stderr, "[DSVP] SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    /* ── Initialize shadercross (must be before GPU device creation) ── */
    if (!SDL_ShaderCross_Init()) {
        fprintf(stderr, "[DSVP] SDL_ShaderCross_Init failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    log_msg("SDL_ShaderCross initialized");

    /* Suppress FFmpeg's internal warnings (container quirks, timestamp
     * heuristics, etc.). In debug builds, keep them visible. */
#ifdef DSVP_DEBUG
    av_log_set_level(AV_LOG_VERBOSE);
#else
    av_log_set_level(AV_LOG_ERROR);
#endif

    /* ── Create window ── */
    SDL_Window *window = SDL_CreateWindow(
        DSVP_WINDOW_TITLE,
        DEFAULT_WIN_W, DEFAULT_WIN_H,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY
    );
    if (!window) {
        fprintf(stderr, "[DSVP] Cannot create window: %s\n", SDL_GetError());
        SDL_ShaderCross_Quit();
        SDL_Quit();
        return 1;
    }

    /* ── Set window icon ── */
    {
        SDL_IOStream *io = SDL_IOFromConstMem(dsvp_icon_bmp, dsvp_icon_bmp_size);
        if (io) {
            SDL_Surface *icon = SDL_LoadBMP_IO(io, true);  /* true = auto-close io */
            if (icon) {
                SDL_SetWindowIcon(window, icon);
                SDL_DestroySurface(icon);
            }
        }
    }

    /* ── Create GPU device ──
     * Force Vulkan on all platforms. SDL_GPU's D3D12 backend has a
     * transfer buffer synchronization bottleneck: SDL_MapGPUTransferBuffer
     * stalls on GPU fences from the previous frame's copy command,
     * adding 30-180ms per frame depending on texture size. On 4K 10-bit
     * content (19.2MB/frame), this made real-time playback impossible.
     * Vulkan's memory model handles transfer buffer cycling without
     * fence stalls, giving ~1-2ms per frame on the same content.*/
    /* D3D12 has transfer buffer fence stalls (30-180ms/frame).
     * Force Vulkan on Windows/Linux, Metal on macOS. */
#ifdef __APPLE__
    SDL_SetHint(SDL_HINT_GPU_DRIVER, "metal");
#else
    SDL_SetHint(SDL_HINT_GPU_DRIVER, "vulkan");
#endif

#ifdef DSVP_DEBUG
    bool gpu_debug = true;
#else
    bool gpu_debug = false;
#endif
    SDL_GPUDevice *gpu_device = SDL_CreateGPUDevice(
        SDL_ShaderCross_GetSPIRVShaderFormats(),
        gpu_debug,
        NULL    /* preferred driver — vulkan (set by hint above) */
    );
    if (!gpu_device) {
        fprintf(stderr, "[DSVP] Cannot create GPU device: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_ShaderCross_Quit();
        SDL_Quit();
        return 1;
    }
    log_msg("GPU device created (driver: %s)",
            SDL_GetGPUDeviceDriver(gpu_device));

    /* ── Claim window for GPU rendering ── */
    if (!SDL_ClaimWindowForGPUDevice(gpu_device, window)) {
        fprintf(stderr, "[DSVP] Cannot claim window for GPU: %s\n", SDL_GetError());
        SDL_DestroyGPUDevice(gpu_device);
        SDL_DestroyWindow(window);
        SDL_ShaderCross_Quit();
        SDL_Quit();
        return 1;
    }

    /* ── Set VSync via swapchain parameters ── */
    SDL_SetGPUSwapchainParameters(gpu_device, window,
        SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
        SDL_GPU_PRESENTMODE_VSYNC);
    log_msg("GPU: swapchain set to SDR + VSync");

    /* ── Initialize subtitle font (Phase 2 will use for GPU overlay) ── */
    if (sub_init_font() < 0) {
        log_msg("WARNING: Subtitle rendering disabled (no font)");
    }

    /* ── Initialize player state ── */
    PlayerState ps;
    memset(&ps, 0, sizeof(ps));
    ps.window     = window;
    ps.gpu_device = gpu_device;
    ps.volume     = 1.00;
    ps.video_stream_idx = -1;
    ps.audio_stream_idx = -1;
    ps.sub_active_idx   = -1;
    ps.win_w = DEFAULT_WIN_W;
    ps.win_h = DEFAULT_WIN_H;
    ps.hdr_target_idx = 0;  /* default: 203 nits (industry standard) */
    ps.gpu_uniforms.hdr_target_nits = 203.0f;
    ps.gpu_uniforms.hdr_midtone_gain = 1.3f;  /* default: moderate midtone lift */

    /* Audio passthrough mode: PCM (default), auto (probe HDMI), passthrough (force).
     * Set via DSVP_AUDIO_MODE environment variable. */
    {
        const char *mode_env = getenv("DSVP_AUDIO_MODE");
        if (mode_env) {
            if (strcmp(mode_env, "auto") == 0) {
                ps.audio_mode = AUDIO_MODE_AUTO;
                log_msg("Audio mode: AUTO (will probe HDMI sink)");
            } else if (strcmp(mode_env, "passthrough") == 0) {
                ps.audio_mode = AUDIO_MODE_PASSTHROUGH;
                log_msg("Audio mode: PASSTHROUGH (force bitstream)");
            } else {
                log_msg("Audio mode: PCM (default)");
            }
        }
    }

    /* ── Compile shaders and create GPU pipelines ── */
    if (gpu_create_pipelines(&ps) < 0) {
        fprintf(stderr, "[DSVP] GPU pipeline creation failed\n");
        SDL_DestroyGPUDevice(gpu_device);
        SDL_DestroyWindow(window);
        SDL_ShaderCross_Quit();
        SDL_Quit();
        return 1;
    }

    /* ── Open file from command line if provided ── */
    if (open_path) {
        if (player_open(&ps, open_path) != 0) {
            log_msg("ERROR: Failed to open: %s", open_path);
        } else {
            reset_gain(&ps);
            playlist_scan(&ps);
        }
        free(open_path);
        open_path = NULL;
    }

    /* ── Main loop ── */
    while (!ps.quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {

            case SDL_EVENT_QUIT:
                if (ps.playing) player_close(&ps);
                ps.quit = 1;
                break;

            case SDL_EVENT_KEY_DOWN:
                /* OS auto-repeat is not a user action: a held Q closed the
                 * file and then the first repeat quit the app; held Space
                 * thrashed the audio device; held N/B fired repeated full
                 * close/open cycles. Volume arrows are the one binding
                 * where hold-to-repeat is wanted (clamped, idempotent). */
                if (ev.key.repeat
                        && ev.key.key != SDLK_UP && ev.key.key != SDLK_DOWN)
                    break;
                switch (ev.key.key) {

                case SDLK_Q:
                    if (ps.playing) {
                        player_close(&ps);
                        ps.quit = 0; /* don't exit, return to idle */
                    } else {
                        ps.quit = 1;
                    }
                    break;

                case SDLK_O: {
                    char path[1024] = {0};
                    log_msg("File open dialog requested");
                    /* Pause audio while dialog blocks the render loop */
                    int was_playing = ps.playing && !ps.paused;
                    if (was_playing && ps.audio_stream)
                        SDL_PauseAudioStreamDevice(ps.audio_stream);
                    if (open_file_dialog(path, sizeof(path))) {
                        log_msg("Opening file: %s", path);
                        if (ps.playing) player_close(&ps);
                        ps.quit = 0;
                        if (player_open(&ps, path) != 0) {
                            log_msg("ERROR: Failed to open: %s", path);
                        } else {
                            reset_gain(&ps);
                            playlist_scan(&ps);
                        }
                    } else {
                        log_msg("File dialog cancelled");
                        /* Resume audio and resync frame timer */
                        if (was_playing && ps.audio_stream) {
                            ps.frame_timer = get_time_sec();
                            SDL_ResumeAudioStreamDevice(ps.audio_stream);
                        }
                    }
                    break;
                }

                case SDLK_SPACE:
                    if (ps.playing) {
                        ps.paused = !ps.paused;
                        if (ps.audio_stream) {
                            if (ps.paused)
                                SDL_PauseAudioStreamDevice(ps.audio_stream);
                            else
                                SDL_ResumeAudioStreamDevice(ps.audio_stream);
                        }
                        if (!ps.paused) {
                            ps.frame_timer = get_time_sec();
                            /* Restart FPS window — the paused gap would
                             * otherwise skew the first reading on resume */
                            ps.fps_window_start   = 0.0;
                            ps.fps_window_frames  = 0;
                            ps.rfps_window_frames = 0;
                        }
                    }
                    break;

                case SDLK_F:
                    toggle_fullscreen(&ps, window);
                    break;

                case SDLK_D:
                    if (ps.playing) {
                        ps.show_debug = !ps.show_debug;
                        if (ps.show_debug) {
                            ps.show_info = 0;  /* mutually exclusive */
                            player_build_debug_info(&ps);
                        }
                    }
                    break;

                case SDLK_I:
                    if (ps.playing) {
                        ps.show_info = !ps.show_info;
                        if (ps.show_info) {
                            ps.show_debug = 0;  /* mutually exclusive */
                            player_build_media_info(&ps);
                        }
                    }
                    break;

                case SDLK_H:
                    if (ps.playing && ps.gpu_uniforms.is_hdr > 0.0f) {
                        int mode = (int)ps.gpu_uniforms.hdr_debug;
                        mode = (mode + 1) % 4;
                        ps.gpu_uniforms.hdr_debug = (float)mode;
                        ps.cache_valid = 0;  /* uniforms changed — re-shade */
                        float tn = ps.gpu_uniforms.hdr_target_nits;
                        const char *fmt[] = {
                            "HDR: BT.2390 (%.0f nit target)",
                            "HDR: BT.2390 (%.0f+100 nit target)",
                            "HDR: PQ bypass (raw stream)",
                            "HDR: Luminance visualization"
                        };
                        snprintf(ps.aud_osd, sizeof(ps.aud_osd), fmt[mode],
                                 mode <= 1 ? tn : 0.0f);
                        ps.aud_osd_until = get_time_sec() + 2.0;
                    }
                    break;

                case SDLK_T:
                    if (ps.playing && ps.gpu_uniforms.is_hdr > 0.0f) {
                        static const float targets[] = { 203.0f, 300.0f, 400.0f };
                        ps.hdr_target_idx = (ps.hdr_target_idx + 1) % 3;
                        ps.gpu_uniforms.hdr_target_nits = targets[ps.hdr_target_idx];
                        ps.cache_valid = 0;  /* uniforms changed — re-shade */
                        snprintf(ps.aud_osd, sizeof(ps.aud_osd),
                                 "SDR target: %.0f nits", targets[ps.hdr_target_idx]);
                        ps.aud_osd_until = get_time_sec() + 2.0;
                        log_msg("HDR: SDR target changed to %.0f nits",
                                targets[ps.hdr_target_idx]);
                    }
                    break;

                case SDLK_G:
                    if (ps.playing && ps.gpu_uniforms.is_hdr > 0.0f) {
                        static const float gains[] = { 1.0f, 1.1f, 1.2f, 1.3f, 1.35f, 1.4f };
                        s_gain_idx = (s_gain_idx + 1) % 6;
                        ps.gpu_uniforms.hdr_midtone_gain = gains[s_gain_idx];
                        ps.cache_valid = 0;  /* uniforms changed — re-shade */
                        snprintf(ps.aud_osd, sizeof(ps.aud_osd),
                                 "Midtone gain: %.2f", gains[s_gain_idx]);
                        ps.aud_osd_until = get_time_sec() + 2.0;
                        log_msg("HDR: midtone gain changed to %.2f",
                                gains[s_gain_idx]);
                    }
                    break;

                case SDLK_S:
                    sub_cycle(&ps);
                    break;

                case SDLK_A:
                    audio_cycle(&ps);
                    break;

                case SDLK_LEFT:
                    player_seek(&ps, -SEEK_STEP_SEC);
                    break;

                case SDLK_RIGHT:
                    player_seek(&ps, SEEK_STEP_SEC);
                    break;

                case SDLK_UP:
                    ps.volume += VOLUME_STEP;
                    if (ps.volume > 1.0) ps.volume = 1.0;
                    if (ps.audio_stream)
                        SDL_SetAudioStreamGain(ps.audio_stream, ps.volume);
                    ps.show_seekbar = 1;
                    ps.seekbar_hide_time = get_time_sec() + 1.5;
                    break;

                case SDLK_DOWN:
                    ps.volume -= VOLUME_STEP;
                    if (ps.volume < 0.0) ps.volume = 0.0;
                    if (ps.audio_stream)
                        SDL_SetAudioStreamGain(ps.audio_stream, ps.volume);
                    ps.show_seekbar = 1;
                    ps.seekbar_hide_time = get_time_sec() + 1.5;
                    break;

                case SDLK_N:  /* Next file in folder */
                case SDLK_B:  /* Previous (Back) file in folder */
                {
                    int delta = (ev.key.key == SDLK_N) ? 1 : -1;
                    if (ps.playlist_count > 0 && ps.playlist_index >= 0) {
                        int next = ps.playlist_index + delta;
                        if (next < 0 || next >= ps.playlist_count) {
                            /* At boundary — show OSD */
                            snprintf(ps.aud_osd, sizeof(ps.aud_osd),
                                     "No %s file in folder",
                                     delta > 0 ? "next" : "previous");
                            ps.aud_osd_until = get_time_sec() + 2.0;
                        } else {
                            /* player_close/player_open touch neither the
                             * playlist nor ps.fullscreen — the save/NULL/
                             * restore dance this used to do guarded against
                             * behavior that does not exist. */
                            player_close(&ps);

                            log_msg("Playlist nav: opening [%d/%d] %s",
                                    next + 1, ps.playlist_count,
                                    ps.playlist_files[next]);

                            /* Index advances even on failure (as before):
                             * the next N/B steps past a bad file instead of
                             * retrying it forever. */
                            ps.playlist_index = next;
                            if (player_open(&ps, ps.playlist_files[next]) == 0) {
                                reset_gain(&ps);
                            } else {
                                log_msg("ERROR: Failed to open: %s",
                                        ps.playlist_files[next]);
                            }
                        }
                    }
                    break;
                }

                default:
                    break;
                }
                break;

            case SDL_EVENT_MOUSE_BUTTON_DOWN: {
                /* Click position in swapchain PHYSICAL pixels.
                 *
                 * HIDPI: the overlay (and the seekbar_track_x/w it exports)
                 * is drawn at swapchain physical resolution, but mouse
                 * coordinates arrive in LOGICAL window units. On HiDPI
                 * displays these differ by the scale factor, so the click
                 * must be scaled into physical space before hit-testing. */
                int lw = 0, lh = 0;
                SDL_GetWindowSize(window, &lw, &lh);
                float mx = ev.button.x;
                float my = ev.button.y;
                if (ps.sc_w > 0 && lw > 0) mx *= (float)ps.sc_w / lw;
                if (ps.sc_h > 0 && lh > 0) my *= (float)ps.sc_h / lh;

                int h_now = (ps.sc_h > 0) ? ps.sc_h : lh;
                /* Same scale rule as overlay.c draws with — one function,
                 * so clicks cannot land where the bar is not. */
                int s = ui_scale_for(&ps, h_now);
                int bar_h = 30 * s;
                int bar_y = h_now - bar_h;
                int on_seekbar = ps.playing && ps.show_seekbar
                                 && my >= bar_y && my <= h_now;

                if (ev.button.button == SDL_BUTTON_LEFT
                        && ev.button.clicks == 2 && !on_seekbar) {
                    /* Double-click outside the seekbar → toggle fullscreen.
                     * On the seekbar this used to fall through into the
                     * hit-test: seek on click 1, then fullscreen AND a
                     * second mid-transition seek on click 2. Now click 2
                     * on the bar does nothing. */
                    toggle_fullscreen(&ps, window);
                }

                /* Click on seek bar — buttons and progress track.
                 * Geometry must match overlay.c draw_seekbar() layout:
                 *   [btn_margin][◀][gap][▶][gap][time][12][==track==][12][sep][vol][margin]
                 * No clicks<2 gate here: the !on_seekbar exclusion on the
                 * fullscreen branch above already prevents the old
                 * double-click fall-through; swallowing the second click
                 * only cost legitimate rapid re-seeks / repeated
                 * prev-next presses. */
                else if (ev.button.button == SDL_BUTTON_LEFT
                        && on_seekbar) {
                    {
                        /* Button geometry (must match overlay.c) */
                        int btn_x = 8 * s;
                        int btn_sz = 8 * s;
                        int btn_gap = 10 * s;
                        int btn2_x = btn_x + btn_sz + btn_gap;


                        /* Prev button click area */
                        if (mx >= btn_x && mx <= btn_x + btn_sz) {
                            SDL_Event fake = {0};
                            fake.type = SDL_EVENT_KEY_DOWN;
                            fake.key.key = SDLK_B;
                            SDL_PushEvent(&fake);
                        }
                        /* Next button click area */
                        else if (mx >= btn2_x && mx <= btn2_x + btn_sz) {
                            SDL_Event fake = {0};
                            fake.type = SDL_EVENT_KEY_DOWN;
                            fake.key.key = SDLK_N;
                            SDL_PushEvent(&fake);
                        }
                        /* Seek track */
                        else {
                            /* seekbar_track_x/w are exported by overlay.c
                             * in physical pixels — same space as mx now */
                            int track_x = ps.seekbar_track_x;
                            int track_w = ps.seekbar_track_w;

                            if (track_w > 20 && mx >= track_x
                                    && mx <= track_x + track_w) {
                                double frac = (double)(mx - track_x) / track_w;
                                if (frac < 0.0) frac = 0.0;
                                if (frac > 1.0) frac = 1.0;
                                double duration = (ps.fmt_ctx->duration != AV_NOPTS_VALUE)
                                    ? (double)ps.fmt_ctx->duration / AV_TIME_BASE : 0.0;
                                double target = frac * duration;
                                /* Audio-only files track position on the
                                 * audio clock, not video_clock */
                                double curpos = (ps.video_stream_idx >= 0)
                                    ? ps.video_clock : ps.audio_clock_sync;
                                player_seek(&ps, target - curpos);
                            }
                        }
                    }
                }
                break;
            }

            case SDL_EVENT_MOUSE_MOTION:
                /* Show overlays on mouse movement, auto-hide after 3s */
                SDL_ShowCursor();
                if (ps.playing) {
                    ps.show_seekbar = 1;
                    ps.seekbar_hide_time = get_time_sec() + 1.5;
                }
                break;

            case SDL_EVENT_AUDIO_DEVICE_REMOVED:
                /* SDL3 migrates default-device streams when the DEFAULT
                 * changes, but a dying endpoint with no fallback (USB DAC
                 * unplugged, BT sink dropped) just stops the callback:
                 * audio_clock_sync froze, video degraded to the capped
                 * slideshow, and EOF never fired. Try to reopen; if no
                 * device exists, drop to video-only. seek_mutex: the demux
                 * seek path touches audio_stream and audio_codec_ctx. */
                if (ps.playing && ps.audio_stream) {
                    SDL_AudioDeviceID gone = ev.adevice.which;
                    SDL_AudioDeviceID ours =
                        SDL_GetAudioStreamDevice(ps.audio_stream);
                    if (ours == 0 || ours == gone) {
                        SDL_LockMutex(ps.seek_mutex);
                        log_msg("Audio: output device removed — reopening");
                        audio_close(&ps);
                        if (audio_open(&ps) == 0) {
                            /* Don't resume into an in-flight seek — its
                             * recovery path owns the resume then. */
                            if (!ps.paused && !ps.seek_request
                                    && !ps.seek_recovering)
                                SDL_ResumeAudioStreamDevice(ps.audio_stream);
                        } else {
                            log_msg("Audio: no output device — continuing video-only");
                            avcodec_free_context(&ps.audio_codec_ctx);
                            ps.audio_stream_idx = -1;
                            /* Packets queued before the device died have
                             * no consumer now — orphaned, they block the
                             * EOF close condition forever (and at
                             * throttle depth stall demux outright). */
                            pq_flush(&ps.audio_pq);
                        }
                        SDL_UnlockMutex(ps.seek_mutex);
                    }
                }
                break;

            case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
                /* Compositor-side transitions (WM keybind, refused request)
                 * desync the cached flag; these events are the truth. */
                if (!ps.fullscreen)
                    log_msg("FS-sync: compositor entered fullscreen (we thought windowed)");
                ps.fullscreen = 1;
                break;

            case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
                if (ps.fullscreen)
                    log_msg("FS-sync: compositor left fullscreen (we thought fullscreen)");
                ps.fullscreen = 0;
                break;

            case SDL_EVENT_WINDOW_RESIZED:
                    ps.win_w = ev.window.data1;
                    ps.win_h = ev.window.data2;
                break;
            }
        }

        /* ── Render ── */
        if (ps.playing && !ps.paused) {
            /* Audio-only: no video_display() runs to refresh swapchain
             * dimensions, so keep sc_w/sc_h current for the overlay
             * renderer (seek bar geometry, click hit-testing). */
            if (ps.video_stream_idx < 0) {
                int pw, phh;
                SDL_GetWindowSizeInPixels(window, &pw, &phh);
                ps.sc_w = pw;
                ps.sc_h = phh;
            }

            /* Decode pending subtitles (still queued for Phase 2) */
            sub_decode_pending(&ps);

            /* ── Render overlays to pixel buffer (before GPU submission) ── */
            overlay_render(&ps);

            /* Hide cursor when seek bar auto-hides */
            if (!ps.show_seekbar && !ps.show_debug && !ps.show_info)
                SDL_HideCursor();

            /* ── Video decode and A/V sync ──
             *
             * Two-tier pacing (unchanged from SDL_Renderer version):
             *   1. frame_timer governs WHEN to show a new frame based
             *      on content frame rate.
             *   2. VSync (via GPU swapchain) governs render loop rate.
             *
             * video_display() handles the full GPU submission:
             *   copy pass (upload planes) → render pass (shader draw) → submit.
             * video_reblit() re-draws the last frame without uploading. */
            double now = get_time_sec();
            int new_frame = 0;
            int decoded_this_tick = 0;

            /* max_catchup caps burst decodes per VSync tick.
             * Kept at 4 for all content: at 1:1 (60fps on 60Hz), the
             * natural (2,0) rhythm self-corrects with max_catchup=4.
             * For heavy content (4K H.264), the decoder occasionally
             * needs bursts of 3-4 to recover after expensive I-frames.
             * max_catchup=2 prevented this, causing accumulated drift
             * that triggered 100+ snap-forwards and multi-second A/V
             * desync. max_catchup=4 is the stall recovery safety cap. */
            int max_catchup = 4;
            while (now >= ps.frame_timer && max_catchup-- > 0) {
                int vret = video_decode_frame(&ps);
                if (vret > 0) {
                    decoded_this_tick++;
                    ps.diag_frames_decoded++;

                    /* Compute inter-frame delay from PTS */
                    double pts_delay = ps.video_clock - ps.frame_last_pts;
                    if (pts_delay <= 0.0 || pts_delay >= 1.0)
                        pts_delay = ps.frame_last_delay;
                    ps.frame_last_pts   = ps.video_clock;
                    ps.frame_last_delay = pts_delay;

                    /* A/V sync adjustment */
                    double delay = pts_delay;
                    double av_diff = 0.0;
                    double av_diff_c = 0.0;
                    int one_to_one = 0;

                    /* Audio can end before video (silent credits, truncated
                     * audio track). When the audio pipeline is starved —
                     * queue dry, SDL stream drained — audio_clock_sync
                     * freezes; syncing against it produced an ever-growing
                     * positive av_diff and the frame_timer cap then metered
                     * out one frame per 100ms: a 10fps slideshow for the
                     * rest of the file. Pace by the video clock while
                     * starved; recovers by itself when packets return. */
                    int audio_live = ps.audio_stream_idx >= 0;
                    if (audio_live && ps.audio_pq.nb_packets == 0
                            && (!ps.audio_stream
                                || SDL_GetAudioStreamQueued(ps.audio_stream) <= 0)
                            && ps.audio_buf_index >= ps.audio_buf_size)
                        audio_live = 0;

                    if (audio_live) {
                        av_diff = ps.video_clock - ps.audio_clock_sync;

                        /* Adaptive bias correction: EMA of av_diff
                         * absorbs systematic OS audio pipeline latency.
                         * Only the catch-up (negative) branch uses the
                         * corrected value — the slow-down (positive)
                         * branch uses raw av_diff to avoid overcorrection. */
                        if (!ps.seek_recovering) {
                            ps.av_bias = ps.av_bias * 0.95 + av_diff * 0.05;
                            ps.av_bias_samples++;
                        }
                        av_diff_c = av_diff;
                        if (ps.av_bias_samples >= 60) {
                            double bias = ps.av_bias;
                            if (bias < -0.200) bias = -0.200;
                            if (bias >  0.200) bias =  0.200;
                            av_diff_c = av_diff - bias;
                        }

                        /* 1:1 VSync pacing: when content frame rate
                         * matches display refresh (~50-60fps), VSync
                         * alone provides the pacing heartbeat. Full
                         * A/V delay correction at 1:1 causes oscillation
                         * because any jitter triggers multi-decode
                         * bunching.
                         *
                         * Instead, once the bias EMA has converged
                         * (~2s of playback), apply a micro-correction:
                         * 2% of the converged bias per frame.  At 50ms
                         * bias this is ~1ms/frame on a 16.67ms period —
                         * too small to cause a tick skip, converges in
                         * ~1 second. */
                        one_to_one = (pts_delay > 0.001
                                      && pts_delay < 0.020);

                        double threshold = fmax(pts_delay, 0.01);
                        if (!one_to_one) {
                            if (av_diff > threshold) {
                                delay = pts_delay + av_diff;
                            } else if (av_diff_c < -threshold) {
                                delay = 0.0;
                            }
                        } else if (ps.av_bias_samples >= 120) {
                            /* Micro-correction: nudge frame_timer toward
                             * audio clock without triggering oscillation */
                            double bias = ps.av_bias;
                            if (bias < -0.200) bias = -0.200;
                            if (bias >  0.200) bias =  0.200;
                            delay = pts_delay + bias * 0.02;
                        }

                        if (!ps.seek_recovering
                                && ps.av_bias_samples >= 30
                                && fabs(av_diff) > fabs(ps.diag_max_av_drift))
                            ps.diag_max_av_drift = av_diff;
                    }

                    /* Minimum delay floor */
                    double min_delay = ps.frame_last_delay * 0.5;
                    if (delay < min_delay)
                        delay = min_delay;

                    ps.frame_timer += delay;
                    new_frame = 1;

                    /* Cap: never let frame_timer get more than 100ms ahead
                     * of wall time.  Post-seek rapid frame consumption
                     * (catch-up drops with delay≈0) can accumulate
                     * frame_timer seconds ahead, causing a prolonged
                     * stall when the burst ends. */
                    if (ps.frame_timer > now + 0.1)
                        ps.frame_timer = now + 0.1;

                    /* Drop frame if video is genuinely behind audio.
                     *
                     * At 1:1 (content fps ≈ display refresh), drops are
                     * DISABLED. VSync provides the pacing heartbeat and
                     * the snap-forward handles genuine stalls. The raw
                     * av_diff at 1:1 includes a fixed pipeline offset
                     * (decode latency + OS audio buffering) that isn't
                     * growing drift — the decoder IS keeping up. Dropping
                     * on that offset replaces smooth 60fps video with a
                     * frozen frame, which is far worse than the offset.
                     *
                     * For non-1:1 content (e.g. 24fps on 60Hz), the
                     * accumulator-based timing needs active correction,
                     * so bias-corrected drops still apply at -50ms.
                     *
                     * Gate on bias convergence (60 samples ≈ 1–2s):
                     * before the EMA stabilizes, av_diff is unreliable
                     * and containers with audio PTS lead (MPEG-PS) would
                     * trigger spurious drops. Modern containers have
                     * near-zero av_diff at startup so this gate is a
                     * no-op for them. */
                    if (!one_to_one && audio_live
                            && !ps.seek_recovering
                            && ps.av_bias_samples >= 60) {
                        double drop_diff = av_diff_c;
                        if (drop_diff < -0.05) {
                            new_frame = 0;
                            ps.diag_frames_dropped++;
                            log_msg("DIAG: frame dropped at %.3fs "
                                    "(A/V drift: %.1fms)",
                                    ps.video_clock, av_diff * 1000.0);
                        }
                    }
                } else {
                    if (vret < 0) {
                        log_msg("Video decode error at clock=%.3f",
                                ps.video_clock);
                    } else if (ps.io_error) {
                        /* Persistent decode failure escalated by the
                         * decode thread — tear down cleanly instead of
                         * freezing on the last frame forever. */
                        log_msg("Playback aborted (decode failure), returning to idle");
                        player_close(&ps);
                        ps.quit = 0;
                    } else if (ps.eof && ps.video_pq.nb_packets == 0
                                    && ps.audio_pq.nb_packets == 0
                                    && (!ps.audio_stream
                                        || SDL_GetAudioStreamQueued(ps.audio_stream) <= 0)) {
                        /* Wait for SDL's queued audio to drain too —
                         * otherwise the tail of the audio (matters for
                         * audio-only playback) is cut off at close. */
                        log_msg("Playback finished, returning to idle");
                        player_close(&ps);
                        ps.quit = 0;
                    }
                    break;
                }
            }

            if (decoded_this_tick > 1) {
                ps.diag_multi_decodes++;
            }

            /* Snap forward on extreme stall — but only when the decoder
             * actually produced frames this tick.  If the queue is empty
             * (e.g. EOF drain or demux lag), snapping is pointless and
             * would fire every tick until player_close runs. */
            if (decoded_this_tick > 0
                    && ps.frame_timer < now - 0.1) {
                ps.frame_timer = now;
                ps.diag_timer_snaps++;
                log_msg("DIAG: frame_timer snapped forward "
                        "(stall recovery at %.3fs)", ps.video_clock);
            }

            /* Display the last decoded frame via GPU */
            if (new_frame) {
                video_display(&ps);
                ps.diag_frames_displayed++;
                ps.fps_window_frames++;   /* real-time FPS: content frame */

                /* Resume from seek: first displayed frame post-seek.
                 *
                 * CRITICAL: re-sync audio clocks to video_clock here.
                 * av_seek_frame lands on a keyframe that may be seconds
                 * away from the seek target. The demux thread pre-sets
                 * both clocks to the target, but the first decoded frame
                 * overwrites video_clock to the actual keyframe PTS.
                 * Without this re-sync:
                 *   Forward seek: video_clock > audio_clock → A/V sync
                 *     computes multi-second delay, freezing the main loop.
                 *   Backward seek: video_clock < audio_clock → massive
                 *     negative drift, burst of frame drops.
                 */
                if (ps.seek_recovering) {
                    ps.seek_recovering = 0;
                    ps.seek_recovering_start = 0.0;
                    ps.frame_timer = get_time_sec();

                    /* Re-sync clocks to the actual first-frame PTS.
                     * av_bias survives the seek — it models output-path
                     * latency, which is position-independent; zeroing it
                     * here caused the post-seek drop/judder burst. */
                    ps.audio_clock      = ps.video_clock;
                    ps.audio_clock_sync = ps.video_clock;
                    ps.audio_pts_floor  = ps.video_clock;
                    ps.frame_last_pts   = ps.video_clock;
                    ps.diag_max_av_drift = 0.0;

                    /* Flush stale audio and resume */
                    if (ps.audio_stream) {
                        SDL_ClearAudioStream(ps.audio_stream);
                        if (!ps.paused)
                            SDL_ResumeAudioStreamDevice(ps.audio_stream);
                    }

                    log_msg("DIAG: seek recovery complete at %.3fs",
                            ps.video_clock);
                }
            }

            /* Audio-only / no-video-frame fallback for seek recovery.
             *
             * The seek_recovering branch above clears only when the
             * first decoded video frame is displayed. For audio-only
             * files (video_stream_idx < 0) there is no video frame to
             * wait for — without this fallback audio would stay paused
             * forever. For video files where the seek lands on an
             * unrecoverable region (corrupt stream, missing keyframe),
             * a 2-second timeout forces resume so the user isn't left
             * in silence.
             *
             * Either branch is mutually exclusive with the in-display
             * recovery — that one already cleared seek_recovering. */
            if (ps.playing && ps.seek_recovering
                    && (ps.video_stream_idx < 0
                        || (ps.seek_recovering_start > 0.0
                            && (now - ps.seek_recovering_start) > 2.0))) {
                ps.seek_recovering = 0;
                ps.seek_recovering_start = 0.0;
                ps.frame_timer = now;
                /* audio_clock was set to seek_pos by the demux thread.
                 * For audio-only this is exactly where we want audio
                 * to resume; for video-timeout it's a best-effort
                 * starting point. */
                ps.audio_clock_sync = ps.audio_clock;
                ps.audio_pts_floor  = ps.audio_clock;
                /* av_bias survives this path too — it models output-path
                 * latency, position-independent by design; zeroing it here
                 * (only) let the post-seek judder burst return on the
                 * timeout-recovery path, inconsistent with the normal
                 * recovery branch above. */
                if (ps.audio_stream) {
                    SDL_ClearAudioStream(ps.audio_stream);
                    if (!ps.paused)
                        SDL_ResumeAudioStreamDevice(ps.audio_stream);
                }
                log_msg("DIAG: seek recovery timeout — forcing audio "
                        "resume at %.3fs (%s)",
                        ps.audio_clock,
                        (ps.video_stream_idx < 0)
                            ? "audio-only file"
                            : "no video frame in 2s");
            }

            /* Periodic diagnostics (every 10 seconds) */
            if (ps.playing && now - ps.diag_last_report >= 10.0) {
                double av_now = (ps.audio_stream_idx >= 0)
                    ? ps.video_clock - ps.audio_clock_sync : 0.0;
                log_msg("DIAG: [%.0fs] decoded=%d displayed=%d "
                        "dropped=%d multi_ticks=%d snaps=%d "
                        "A/V=%.1fms peak=%.1fms bias=%.1fms",
                        ps.video_clock,
                        ps.diag_frames_decoded,
                        ps.diag_frames_displayed,
                        ps.diag_frames_dropped,
                        ps.diag_multi_decodes,
                        ps.diag_timer_snaps,
                        av_now * 1000.0,
                        ps.diag_max_av_drift * 1000.0,
                        ps.av_bias * 1000.0);
                ps.diag_last_report = now;
            }

            /* Re-blit on ticks with no new frame (GPU double-buffering) */
            if (!new_frame && ps.playing && ps.gpu_tex_y && ps.video_ready) {
                video_reblit(&ps);
            }

            /* Audio-only playback: no video texture exists, so neither
             * video_display nor video_reblit ever runs. Present the dark
             * background + overlay (seek bar, OSD, info panels) instead. */
            if (ps.playing && ps.video_stream_idx < 0) {
                gpu_submit_background(&ps);
            }

            /* ── Real-time FPS measurement (debug overlay) ──
             * One present happens per playing tick (display, reblit, or
             * audio-only background). Roll a 0.5s window: long enough to
             * be stable, short enough to track seeks and stalls. */
            if (ps.playing) {
                ps.rfps_window_frames++;
                if (ps.fps_window_start <= 0.0)
                    ps.fps_window_start = now;
                double fps_dt = now - ps.fps_window_start;
                if (fps_dt >= 0.5) {
                    ps.fps_content = ps.fps_window_frames  / fps_dt;
                    ps.fps_render  = ps.rfps_window_frames / fps_dt;
                    ps.fps_window_frames  = 0;
                    ps.rfps_window_frames = 0;
                    ps.fps_window_start   = now;
                }
            }

            /* If playback ended this tick (player_close was called in the
             * decode loop above), draw idle immediately so the swapchain
             * gets a frame.  Without this, one tick has no GPU submission
             * and some compositors (Gamescope/Steam Deck) show a stale
             * buffer instead of the last presented frame. */
            if (!ps.playing) {
                gpu_draw_idle(&ps);
                SDL_ShowCursor();
            }


        } else if (ps.playing && ps.paused) {
            /* Paused — decode pending subs, render overlays, redraw current frame */
            if (ps.video_stream_idx < 0) {
                /* Audio-only: refresh physical dims (see playing branch) */
                int pw, phh;
                SDL_GetWindowSizeInPixels(window, &pw, &phh);
                ps.sc_w = pw;
                ps.sc_h = phh;
            }
            sub_decode_pending(&ps);
            overlay_render(&ps);
            if (!ps.show_seekbar && !ps.show_debug && !ps.show_info)
                SDL_HideCursor();
            if (ps.gpu_tex_y) {
                video_reblit(&ps);
            } else if (ps.video_stream_idx < 0) {
                /* Audio-only: no video texture — background + overlay */
                gpu_submit_background(&ps);
            }
        } else {
            /* No media loaded — draw idle screen */
            gpu_draw_idle(&ps);
            SDL_ShowCursor();
        }

        /* Don't burn CPU when idle, paused, or in audio-only playback
         * (no video pacing loop; VSync on the background present plus
         * this delay keeps the loop at a sane rate). */
        if (!ps.playing || ps.paused || ps.video_stream_idx < 0) {
            SDL_Delay(16); /* ~60fps idle */
        }
    }

    /* ── Cleanup ── */
    log_msg("Shutting down");
    if (ps.playing) player_close(&ps);
    playlist_free(&ps);
    overlay_cleanup();
    sub_close_font();
    gpu_destroy_pipelines(&ps);
    SDL_ReleaseWindowFromGPUDevice(gpu_device, window);
    SDL_DestroyGPUDevice(gpu_device);
    SDL_DestroyWindow(window);
    SDL_ShaderCross_Quit();
    SDL_Quit();
    log_close();

    return 0;
}
