/*
 * DSVP — Dead Simple Video Player
 * log.c — Crash-safe file logger
 *
 * Writes to dsvp.log next to the executable (resolved, not assumed:
 * the old CWD-relative open meant a file-association launch dropped a
 * dsvp.log into every media folder the user opened, and a Start-Menu
 * launch from Program Files silently failed to create one at all).
 * Falls back to the CWD if the executable's directory is unwritable
 * (system-wide installs), then to stderr-only.
 *
 * Every write is flushed immediately so the log survives hard crashes.
 * Each message is formatted once and emitted with ONE stdio call per
 * sink — the demux, decode, audio-callback, and main threads all log,
 * and three calls per line let them splice each other's output in
 * exactly the seek-storm logs used for diagnosis.
 */

#include "dsvp.h"

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <unistd.h>
#endif

static FILE *g_logfile = NULL;

/* Resolve <executable dir>/dsvp.log into out. Returns 0 on failure. */
static int exe_relative_log_path(char *out, size_t out_size) {
#ifdef _WIN32
    wchar_t wpath[2048];
    DWORD n = GetModuleFileNameW(NULL, wpath, 2048);
    if (n == 0 || n >= 2048) return 0;
    char path[4096];
    int len = WideCharToMultiByte(CP_UTF8, 0, wpath, -1,
                                  path, sizeof(path), NULL, NULL);
    if (len <= 0) return 0;
    char *sep = strrchr(path, '\\');
    if (!sep) return 0;
    *sep = '\0';
    snprintf(out, out_size, "%s\\dsvp.log", path);
    return 1;
#else
    char path[4096];
    ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (n <= 0) return 0;
    path[n] = '\0';
    char *sep = strrchr(path, '/');
    if (!sep) return 0;
    *sep = '\0';
    snprintf(out, out_size, "%s/dsvp.log", path);
    return 1;
#endif
}

void log_init(void) {
    char path[4352];
    if (exe_relative_log_path(path, sizeof(path)))
        g_logfile = fopen(path, "w");
    if (!g_logfile)
        g_logfile = fopen("dsvp.log", "w");  /* unwritable install dir */
    if (g_logfile) {
        /* Disable buffering — every write goes to disk immediately */
        setvbuf(g_logfile, NULL, _IONBF, 0);
        log_msg("=== DSVP %s started ===", DSVP_VERSION);
    }
}

void log_close(void) {
    if (g_logfile) {
        log_msg("=== DSVP shutdown ===");
        fclose(g_logfile);
        g_logfile = NULL;
    }
}

void log_msg(const char *fmt, ...) {
    va_list args;
    char line[2048];
    double t = get_time_sec();

    int off = snprintf(line, sizeof(line), "[%10.3f] ", t);
    va_start(args, fmt);
    vsnprintf(line + off, sizeof(line) - (size_t)off - 1, fmt, args);
    va_end(args);
    size_t len = strlen(line);
    line[len]     = '\n';
    line[len + 1] = '\0';

    if (g_logfile)
        fwrite(line, 1, len + 1, g_logfile);

    /* stderr gets its own prefix — still one write call */
    char eline[2064];
    int elen = snprintf(eline, sizeof(eline), "[DSVP%s", line + 1);
    if (elen > 0)
        fwrite(eline, 1, (size_t)elen, stderr);
}
