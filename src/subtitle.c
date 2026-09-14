/*
 * DSVP — Dead Simple Video Player
 * subtitle.c — Subtitle stream detection, decoding, and rendering
 *
 * Handles:
 *   - Cataloging available subtitle tracks in a container
 *   - Opening/closing subtitle codecs
 *   - Decoding text subtitles (SRT, ASS/SSA)
 *   - Rendering with SDL_ttf: golden yellow (#FFDF00) + black outline
 *   - ASS/SSA typesetting through libass when built with it: the
 *     author's styles, fonts (embedded attachments included),
 *     positioning and animation, composited by overlay.c
 *   - Track cycling with 'S' key (including "Off" option)
 */

#include "dsvp.h"
#include <limits.h>
#include <zlib.h>

/* ── Verbose per-packet logging ────────────────────────────────────────
 *
 * log_msg() writes are fully UNBUFFERED (file + stderr) and the verbose
 * subtitle logs below fire on the MAIN thread for every queued packet —
 * including bulk drains when cycling subtitle tracks. That blocking I/O
 * in the render loop was a measurable contributor to a 312-frame-drop
 * incident during a subtitle cycle on 4K content. Compile them out of
 * release builds; state-change and error logs stay unconditional. */
#ifdef DSVP_DEBUG
  #define sub_vlog(...) log_msg(__VA_ARGS__)
#else
  #define sub_vlog(...) ((void)0)
#endif

/* ── Font state (module-level) ─────────────────────────────────────── */

static TTF_Font *sub_font             = NULL;
static TTF_Font *sub_font_outline     = NULL;
static TTF_Font *sub_font_cjk         = NULL;
static TTF_Font *sub_font_cjk_outline = NULL;
static int       font_loaded          = 0;

/* Extended-script fallback chain — covers Arabic, Hebrew, Indic scripts,
 * SE Asian (Thai, Lao, Khmer, Myanmar), Georgian, Armenian, Ethiopic,
 * Tibetan, and others.  Each script tries platform-specific paths in
 * order; first match per script attaches.  Path-dedup prevents double-
 * attaching shared fonts (e.g. arial.ttf covers both Arabic and Hebrew
 * on Windows; Nirmala UI covers most Indic scripts on Windows). */
#define MAX_EXTENDED_FALLBACKS  32
#define EXTENDED_FONT_PATH_MAX  256
static TTF_Font *sub_font_extended[MAX_EXTENDED_FALLBACKS];
static TTF_Font *sub_font_extended_outline[MAX_EXTENDED_FALLBACKS];
static char      sub_font_extended_paths[MAX_EXTENDED_FALLBACKS][EXTENDED_FONT_PATH_MAX];
static int       sub_font_extended_count = 0;

/* ── Font discovery ────────────────────────────────────────────────── */

static const char *find_system_font(void) {
    static const char *candidates[] = {
#ifdef _WIN32
        "C:\\Windows\\Fonts\\verdana.ttf",
        "C:\\Windows\\Fonts\\arial.ttf",
        "C:\\Windows\\Fonts\\tahoma.ttf",
        "C:\\Windows\\Fonts\\segoeui.ttf",
#elif defined(__APPLE__)
        "/System/Library/Fonts/Supplemental/Verdana.ttf",
        "/System/Library/Fonts/Helvetica.ttc",
        "/Library/Fonts/Arial.ttf",
#else
        "/usr/share/fonts/truetype/msttcorefonts/Verdana.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/liberation-sans/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/google-noto/NotoSans-Regular.ttf",
#endif
        NULL
    };

    for (int i = 0; candidates[i]; i++) {
        FILE *f = fopen(candidates[i], "rb");
        if (f) {
            fclose(f);
            return candidates[i];
        }
    }
    return NULL;
}

static const char *find_cjk_font(void) {
    static const char *candidates[] = {
#ifdef _WIN32
        "C:\\Windows\\Fonts\\msgothic.ttc",
        "C:\\Windows\\Fonts\\msyh.ttc",
        "C:\\Windows\\Fonts\\msjh.ttc",
        "C:\\Windows\\Fonts\\yugothm.ttc",
#elif defined(__APPLE__)
        "/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc",
        "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
#else
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/OTF/NotoSansCJK-Regular.ttc",
#endif
        NULL
    };

    for (int i = 0; candidates[i]; i++) {
        FILE *f = fopen(candidates[i], "rb");
        if (f) {
            fclose(f);
            return candidates[i];
        }
    }
    return NULL;
}


/* ── Extended script fallback helpers ──────────────────────────────── */

/* Attach a font as a fallback for both regular and outline subtitle fonts.
 * Returns 1 if newly attached (or already attached via dedup), 0 on miss.
 * Silently no-ops if MAX_EXTENDED_FALLBACKS is reached.
 *
 * Two-step open: the same path is opened twice — once for the regular
 * font chain, once for the outlined font chain.  SDL_ttf's outline mode
 * is a per-font property, not per-render, so each chain needs its own
 * font handle. */
static int try_attach_extended_fallback(const char *path, int font_size) {
    if (sub_font_extended_count >= MAX_EXTENDED_FALLBACKS) return 0;

    /* Existence probe before TTF_OpenFont — avoids noisy SDL errors on
     * platforms where most candidate paths legitimately don't exist. */
    FILE *probe = fopen(path, "rb");
    if (!probe) return 0;
    fclose(probe);

    /* Path-dedup: shared fonts (arial.ttf for Arabic+Hebrew on Windows,
     * Nirmala.ttf for the Indic family) appear in multiple script tables.
     * Attach the file once, let SDL_ttf reuse it across scripts. */
    for (int i = 0; i < sub_font_extended_count; i++) {
        if (strcmp(sub_font_extended_paths[i], path) == 0) {
            return 1;
        }
    }

    TTF_Font *font = TTF_OpenFont(path, font_size);
    if (!font) return 0;
    TTF_SetFontHinting(font, TTF_HINTING_LIGHT);
    TTF_AddFallbackFont(sub_font, font);
    sub_font_extended[sub_font_extended_count] = font;

    TTF_Font *outline = TTF_OpenFont(path, font_size);
    if (outline) {
        TTF_SetFontOutline(outline, 2);
        TTF_SetFontHinting(outline, TTF_HINTING_LIGHT);
        TTF_AddFallbackFont(sub_font_outline, outline);
    }
    sub_font_extended_outline[sub_font_extended_count] = outline;

    snprintf(sub_font_extended_paths[sub_font_extended_count],
             EXTENDED_FONT_PATH_MAX, "%s", path);

    sub_font_extended_count++;
    log_msg("Extended fallback loaded: %s", path);
    return 1;
}

/* Try each path in the NULL-terminated `paths` array; first existing
 * match is attached as a fallback.  Returns 1 if attached, 0 if all missed. */
static int try_attach_script(const char *const paths[], int font_size) {
    for (int i = 0; paths[i]; i++) {
        if (try_attach_extended_fallback(paths[i], font_size))
            return 1;
    }
    return 0;
}


/* ═══════════════════════════════════════════════════════════════════
 * Font Init / Close
 * ═══════════════════════════════════════════════════════════════════ */

int sub_init_font(void) {
    if (font_loaded) return 0;

    if (!TTF_Init()) {
        log_msg("ERROR: TTF_Init failed: %s", SDL_GetError());
        return -1;
    }

    const char *font_path = find_system_font();
    if (!font_path) {
        log_msg("ERROR: No suitable TTF font found on system");
        log_msg("  Windows: needs Verdana or Arial in C:\\Windows\\Fonts\\");
        log_msg("  Linux: sudo apt install fonts-dejavu-core");
        TTF_Quit();
        return -1;
    }

    int font_size = 32;

    sub_font = TTF_OpenFont(font_path, font_size);
    if (!sub_font) {
        log_msg("ERROR: Cannot open font %s: %s", font_path, SDL_GetError());
        TTF_Quit();
        return -1;
    }

    sub_font_outline = TTF_OpenFont(font_path, font_size);
    if (sub_font_outline) {
        TTF_SetFontOutline(sub_font_outline, 2);
    }

    TTF_SetFontHinting(sub_font, TTF_HINTING_LIGHT);
    if (sub_font_outline)
        TTF_SetFontHinting(sub_font_outline, TTF_HINTING_LIGHT);

    /* Try to attach CJK fallback font for Chinese/Japanese/Korean glyphs */
    const char *cjk_path = find_cjk_font();
    if (cjk_path) {
        sub_font_cjk = TTF_OpenFont(cjk_path, font_size);
        if (sub_font_cjk) {
            TTF_SetFontHinting(sub_font_cjk, TTF_HINTING_LIGHT);
            TTF_AddFallbackFont(sub_font, sub_font_cjk);

            sub_font_cjk_outline = TTF_OpenFont(cjk_path, font_size);
            if (sub_font_cjk_outline) {
                TTF_SetFontOutline(sub_font_cjk_outline, 2);
                TTF_SetFontHinting(sub_font_cjk_outline, TTF_HINTING_LIGHT);
                TTF_AddFallbackFont(sub_font_outline, sub_font_cjk_outline);
            }
            log_msg("CJK fallback font loaded: %s", cjk_path);
        }
    }

    /* ── Extended-script fallback chain ─────────────────────────────────
     *
     * Each script tries a NULL-terminated platform-specific path list;
     * first match wins per script.  Shared fonts (arial.ttf on Windows
     * for Arabic+Hebrew, Nirmala.ttf for the Indic family) attach once
     * via path-dedup inside try_attach_extended_fallback().
     *
     * Coverage rationale: SDL_ttf iterates fallbacks lazily per glyph,
     * so missing scripts cost nothing at render time.  Loading more
     * fallbacks costs only a few MB of startup memory per attached font. */

    /* Arabic (also reused for languages using Arabic script: Persian, Urdu) */
    static const char *const arabic_paths[] = {
#ifdef _WIN32
        "C:\\Windows\\Fonts\\arial.ttf",
        "C:\\Windows\\Fonts\\tahoma.ttf",
#elif defined(__APPLE__)
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/System/Library/Fonts/GeezaPro.ttc",
#else
        "/usr/share/fonts/truetype/noto/NotoSansArabic-Regular.ttf",
        "/usr/share/fonts/noto/NotoSansArabic-Regular.ttf",
        "/usr/share/fonts/google-noto/NotoSansArabic-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoNaskhArabic-Regular.ttf",
        "/usr/share/fonts/TTF/NotoSansArabic-Regular.ttf",
#endif
        NULL
    };
    try_attach_script(arabic_paths, font_size);

    /* Hebrew */
    static const char *const hebrew_paths[] = {
#ifdef _WIN32
        "C:\\Windows\\Fonts\\arial.ttf",          /* shared with Arabic, dedups */
        "C:\\Windows\\Fonts\\tahoma.ttf",
#elif defined(__APPLE__)
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/System/Library/Fonts/ArialHB.ttc",
#else
        "/usr/share/fonts/truetype/noto/NotoSansHebrew-Regular.ttf",
        "/usr/share/fonts/noto/NotoSansHebrew-Regular.ttf",
        "/usr/share/fonts/google-noto/NotoSansHebrew-Regular.ttf",
#endif
        NULL
    };
    try_attach_script(hebrew_paths, font_size);

    /* Devanagari (Hindi, Marathi, Nepali, Sanskrit) */
    static const char *const devanagari_paths[] = {
#ifdef _WIN32
        "C:\\Windows\\Fonts\\Nirmala.ttf",
        "C:\\Windows\\Fonts\\mangal.ttf",
#elif defined(__APPLE__)
        "/System/Library/Fonts/Supplemental/DevanagariMT.ttc",
        "/System/Library/Fonts/Supplemental/Kohinoor.ttc",
#else
        "/usr/share/fonts/truetype/noto/NotoSansDevanagari-Regular.ttf",
        "/usr/share/fonts/noto/NotoSansDevanagari-Regular.ttf",
        "/usr/share/fonts/google-noto/NotoSansDevanagari-Regular.ttf",
#endif
        NULL
    };
    try_attach_script(devanagari_paths, font_size);

    /* Bengali (Bangla, Assamese) */
    static const char *const bengali_paths[] = {
#ifdef _WIN32
        "C:\\Windows\\Fonts\\Nirmala.ttf",        /* shared Indic, dedups */
        "C:\\Windows\\Fonts\\vrinda.ttf",
#elif defined(__APPLE__)
        "/System/Library/Fonts/Supplemental/Bangla MN.ttc",
#else
        "/usr/share/fonts/truetype/noto/NotoSansBengali-Regular.ttf",
        "/usr/share/fonts/noto/NotoSansBengali-Regular.ttf",
#endif
        NULL
    };
    try_attach_script(bengali_paths, font_size);

    /* Tamil */
    static const char *const tamil_paths[] = {
#ifdef _WIN32
        "C:\\Windows\\Fonts\\Nirmala.ttf",
        "C:\\Windows\\Fonts\\latha.ttf",
#elif defined(__APPLE__)
        "/System/Library/Fonts/Supplemental/Tamil MN.ttc",
#else
        "/usr/share/fonts/truetype/noto/NotoSansTamil-Regular.ttf",
        "/usr/share/fonts/noto/NotoSansTamil-Regular.ttf",
#endif
        NULL
    };
    try_attach_script(tamil_paths, font_size);

    /* Telugu */
    static const char *const telugu_paths[] = {
#ifdef _WIN32
        "C:\\Windows\\Fonts\\Nirmala.ttf",
        "C:\\Windows\\Fonts\\gautami.ttf",
#elif defined(__APPLE__)
        "/System/Library/Fonts/Supplemental/Telugu MN.ttc",
#else
        "/usr/share/fonts/truetype/noto/NotoSansTelugu-Regular.ttf",
        "/usr/share/fonts/noto/NotoSansTelugu-Regular.ttf",
#endif
        NULL
    };
    try_attach_script(telugu_paths, font_size);

    /* Kannada */
    static const char *const kannada_paths[] = {
#ifdef _WIN32
        "C:\\Windows\\Fonts\\Nirmala.ttf",
        "C:\\Windows\\Fonts\\tunga.ttf",
#elif defined(__APPLE__)
        "/System/Library/Fonts/Supplemental/Kannada MN.ttc",
#else
        "/usr/share/fonts/truetype/noto/NotoSansKannada-Regular.ttf",
        "/usr/share/fonts/noto/NotoSansKannada-Regular.ttf",
#endif
        NULL
    };
    try_attach_script(kannada_paths, font_size);

    /* Malayalam */
    static const char *const malayalam_paths[] = {
#ifdef _WIN32
        "C:\\Windows\\Fonts\\Nirmala.ttf",
        "C:\\Windows\\Fonts\\kartika.ttf",
#elif defined(__APPLE__)
        "/System/Library/Fonts/Supplemental/Malayalam MN.ttc",
#else
        "/usr/share/fonts/truetype/noto/NotoSansMalayalam-Regular.ttf",
        "/usr/share/fonts/noto/NotoSansMalayalam-Regular.ttf",
#endif
        NULL
    };
    try_attach_script(malayalam_paths, font_size);

    /* Gujarati */
    static const char *const gujarati_paths[] = {
#ifdef _WIN32
        "C:\\Windows\\Fonts\\Nirmala.ttf",
        "C:\\Windows\\Fonts\\shruti.ttf",
#elif defined(__APPLE__)
        "/System/Library/Fonts/Supplemental/Gujarati MT.ttc",
#else
        "/usr/share/fonts/truetype/noto/NotoSansGujarati-Regular.ttf",
        "/usr/share/fonts/noto/NotoSansGujarati-Regular.ttf",
#endif
        NULL
    };
    try_attach_script(gujarati_paths, font_size);

    /* Gurmukhi (Punjabi) */
    static const char *const gurmukhi_paths[] = {
#ifdef _WIN32
        "C:\\Windows\\Fonts\\Nirmala.ttf",
        "C:\\Windows\\Fonts\\raavi.ttf",
#elif defined(__APPLE__)
        "/System/Library/Fonts/Supplemental/Gurmukhi MN.ttc",
#else
        "/usr/share/fonts/truetype/noto/NotoSansGurmukhi-Regular.ttf",
        "/usr/share/fonts/noto/NotoSansGurmukhi-Regular.ttf",
#endif
        NULL
    };
    try_attach_script(gurmukhi_paths, font_size);

    /* Oriya */
    static const char *const oriya_paths[] = {
#ifdef _WIN32
        "C:\\Windows\\Fonts\\Nirmala.ttf",
        "C:\\Windows\\Fonts\\kalinga.ttf",
#else
        "/usr/share/fonts/truetype/noto/NotoSansOriya-Regular.ttf",
        "/usr/share/fonts/noto/NotoSansOriya-Regular.ttf",
#endif
        NULL
    };
    try_attach_script(oriya_paths, font_size);

    /* Sinhala */
    static const char *const sinhala_paths[] = {
#ifdef _WIN32
        "C:\\Windows\\Fonts\\Nirmala.ttf",
        "C:\\Windows\\Fonts\\iskpota.ttf",
#elif defined(__APPLE__)
        "/System/Library/Fonts/Supplemental/Sinhala MN.ttc",
#else
        "/usr/share/fonts/truetype/noto/NotoSansSinhala-Regular.ttf",
        "/usr/share/fonts/noto/NotoSansSinhala-Regular.ttf",
#endif
        NULL
    };
    try_attach_script(sinhala_paths, font_size);

    /* Thai */
    static const char *const thai_paths[] = {
#ifdef _WIN32
        "C:\\Windows\\Fonts\\Leelawui.ttf",
        "C:\\Windows\\Fonts\\tahoma.ttf",          /* shared with RTL set, dedups */
#elif defined(__APPLE__)
        "/System/Library/Fonts/Supplemental/Ayuthaya.ttf",
        "/System/Library/Fonts/Supplemental/Thonburi.ttc",
#else
        "/usr/share/fonts/truetype/noto/NotoSansThai-Regular.ttf",
        "/usr/share/fonts/noto/NotoSansThai-Regular.ttf",
#endif
        NULL
    };
    try_attach_script(thai_paths, font_size);

    /* Lao */
    static const char *const lao_paths[] = {
#ifdef _WIN32
        "C:\\Windows\\Fonts\\Leelawui.ttf",
        "C:\\Windows\\Fonts\\Phagspa.ttf",
#elif defined(__APPLE__)
        "/System/Library/Fonts/Supplemental/Lao MN.ttc",
#else
        "/usr/share/fonts/truetype/noto/NotoSansLao-Regular.ttf",
        "/usr/share/fonts/noto/NotoSansLao-Regular.ttf",
#endif
        NULL
    };
    try_attach_script(lao_paths, font_size);

    /* Khmer */
    static const char *const khmer_paths[] = {
#ifdef _WIN32
        "C:\\Windows\\Fonts\\Leelawui.ttf",
        "C:\\Windows\\Fonts\\daunpenh.ttf",
        "C:\\Windows\\Fonts\\khmerui.ttf",
#elif defined(__APPLE__)
        "/System/Library/Fonts/Supplemental/Khmer MN.ttc",
#else
        "/usr/share/fonts/truetype/noto/NotoSansKhmer-Regular.ttf",
        "/usr/share/fonts/noto/NotoSansKhmer-Regular.ttf",
#endif
        NULL
    };
    try_attach_script(khmer_paths, font_size);

    /* Myanmar (Burmese) */
    static const char *const myanmar_paths[] = {
#ifdef _WIN32
        "C:\\Windows\\Fonts\\Mmrtext.ttf",
#elif defined(__APPLE__)
        "/System/Library/Fonts/Supplemental/Myanmar MN.ttc",
#else
        "/usr/share/fonts/truetype/noto/NotoSansMyanmar-Regular.ttf",
        "/usr/share/fonts/noto/NotoSansMyanmar-Regular.ttf",
#endif
        NULL
    };
    try_attach_script(myanmar_paths, font_size);

    /* Georgian */
    static const char *const georgian_paths[] = {
#ifdef _WIN32
        "C:\\Windows\\Fonts\\sylfaen.ttf",
#elif defined(__APPLE__)
        "/Library/Fonts/Helvetica.ttc",
#else
        "/usr/share/fonts/truetype/noto/NotoSansGeorgian-Regular.ttf",
        "/usr/share/fonts/noto/NotoSansGeorgian-Regular.ttf",
#endif
        NULL
    };
    try_attach_script(georgian_paths, font_size);

    /* Armenian */
    static const char *const armenian_paths[] = {
#ifdef _WIN32
        "C:\\Windows\\Fonts\\sylfaen.ttf",         /* shared with Georgian, dedups */
#elif defined(__APPLE__)
        "/Library/Fonts/Mshtakan.ttc",
#else
        "/usr/share/fonts/truetype/noto/NotoSansArmenian-Regular.ttf",
        "/usr/share/fonts/noto/NotoSansArmenian-Regular.ttf",
#endif
        NULL
    };
    try_attach_script(armenian_paths, font_size);

    /* Ethiopic (Amharic, Tigrinya, Ge'ez) */
    static const char *const ethiopic_paths[] = {
#ifdef _WIN32
        "C:\\Windows\\Fonts\\Ebrima.ttf",
        "C:\\Windows\\Fonts\\Nyala.ttf",
#elif defined(__APPLE__)
        "/Library/Fonts/Kefa.ttc",
#else
        "/usr/share/fonts/truetype/noto/NotoSansEthiopic-Regular.ttf",
        "/usr/share/fonts/noto/NotoSansEthiopic-Regular.ttf",
#endif
        NULL
    };
    try_attach_script(ethiopic_paths, font_size);

    /* Tibetan */
    static const char *const tibetan_paths[] = {
#ifdef _WIN32
        "C:\\Windows\\Fonts\\himalaya.ttf",
#elif defined(__APPLE__)
        "/Library/Fonts/Kailasa.ttc",
#else
        "/usr/share/fonts/truetype/noto/NotoSansTibetan-Regular.ttf",
        "/usr/share/fonts/noto/NotoSansTibetan-Regular.ttf",
#endif
        NULL
    };
    try_attach_script(tibetan_paths, font_size);

    if (sub_font_extended_count > 0) {
        log_msg("Extended script fallbacks: %d font(s) loaded", sub_font_extended_count);
    } else {
        log_msg("Extended script fallbacks: none found "
                "(scripts beyond Latin/CJK may render as boxes)");
    }

    font_loaded = 1;
    log_msg("Subtitle font loaded: %s (%dpt)", font_path, font_size);
    return 0;
}

void sub_close_font(void) {
    /* Free extended-script fallbacks (added via TTF_AddFallbackFont).
     * Close before the chain root so SDL_ttf doesn't end up holding
     * references to a freed primary font. */
    for (int i = 0; i < sub_font_extended_count; i++) {
        if (sub_font_extended[i])         { TTF_CloseFont(sub_font_extended[i]);         sub_font_extended[i] = NULL; }
        if (sub_font_extended_outline[i]) { TTF_CloseFont(sub_font_extended_outline[i]); sub_font_extended_outline[i] = NULL; }
        sub_font_extended_paths[i][0] = '\0';
    }
    sub_font_extended_count = 0;

    if (sub_font_cjk)         { TTF_CloseFont(sub_font_cjk);         sub_font_cjk = NULL; }
    if (sub_font_cjk_outline) { TTF_CloseFont(sub_font_cjk_outline); sub_font_cjk_outline = NULL; }
    if (sub_font)         { TTF_CloseFont(sub_font);         sub_font = NULL; }
    if (sub_font_outline) { TTF_CloseFont(sub_font_outline); sub_font_outline = NULL; }
    if (font_loaded)      { TTF_Quit(); font_loaded = 0; }
}

/* Font accessors for overlay.c (GPU-composited subtitle rendering) */
TTF_Font *sub_get_font(void)         { return sub_font; }
TTF_Font *sub_get_outline_font(void) { return sub_font_outline; }

/* Resize the whole font chain, fallbacks included. SDL3_ttf renders a
 * missing glyph at the FALLBACK font's own size, so leaving the CJK and
 * extended-script fonts at their 32pt open size gave mixed-script lines
 * mismatched glyph heights at every other window size. */
void sub_set_font_size(int font_size) {
    if (sub_font)             TTF_SetFontSize(sub_font, font_size);
    if (sub_font_outline)     TTF_SetFontSize(sub_font_outline, font_size);
    if (sub_font_cjk)         TTF_SetFontSize(sub_font_cjk, font_size);
    if (sub_font_cjk_outline) TTF_SetFontSize(sub_font_cjk_outline, font_size);
    for (int i = 0; i < MAX_EXTENDED_FALLBACKS; i++) {
        if (sub_font_extended[i])
            TTF_SetFontSize(sub_font_extended[i], font_size);
        if (sub_font_extended_outline[i])
            TTF_SetFontSize(sub_font_extended_outline[i], font_size);
    }
}

/* Free any active bitmap subtitle data */
static void sub_clear_bitmaps(PlayerState *ps) {
    for (int i = 0; i < ps->sub_bitmap_count; i++) {
        if (ps->sub_bitmap_data[i]) {
            av_free(ps->sub_bitmap_data[i]);
            ps->sub_bitmap_data[i] = NULL;
        }
    }
    ps->sub_bitmap_count = 0;
}

/* Everything on screen goes: active text cues, bitmap rects, the
 * joined text, the valid flag. MAIN THREAD ONLY (frees bitmap rects
 * the overlay may be uploading). */
void sub_clear_display(PlayerState *ps) {
    ps->sub_cue_count = 0;
    ps->sub_valid     = 0;
    ps->sub_text[0]   = '\0';
    sub_clear_bitmaps(ps);
}


/* ═══════════════════════════════════════════════════════════════════
 * libass — ASS/SSA typesetting (2026-09 cycle)
 *
 * FOR: put on screen what the subtitle author typeset — a sign
 * translation over the sign, an SFX over the kanji, in the author's
 * font — instead of the stripped plain text at the bottom.
 *
 * Lifecycle: ASS_Library + ASS_Renderer per FILE, created lazily the
 * first time an ASS/SSA track is opened (the file's font attachments
 * are handed to the library then, and ass_set_fonts builds the font
 * provider — fontconfig on Linux, DirectWrite on Windows). ASS_Track
 * per OPENED stream: the codec's extradata is the script header
 * (styles, PlayRes), every packet is one Matroska-form event fed by
 * ass_process_chunk with its PTS/duration in ms. Rendering happens in
 * overlay.c through sub_ass_render() at the frame's output size.
 *
 * Threads: every libass call here runs on the MAIN thread — the drain
 * (under seek_mutex), the overlay draw, the S-cycle open/close and
 * player_close. The demux thread's seek handler never touches libass;
 * it flushes the packet queue and the AVCodecContext only. No
 * ass_flush_events on seek: events are timestamped and libass drops
 * duplicates by ReadOrder, so a backward seek re-feeding the queue is
 * harmless and a forward seek simply has a gap that the demuxer
 * refills. Memory is bounded by the file's event count.
 *
 * Off switches: NO_LIBASS=1 at build time; DSVP_NO_LIBASS=1 at run
 * time (falsification: same binary, the pre-libass stripped path).
 * ═══════════════════════════════════════════════════════════════════ */

#ifdef DSVP_HAVE_LIBASS

#define SUB_ASS_MSG_CAP  32   /* libass errors/warnings logged per file */
#define SUB_ASS_INFO_CAP 24   /* libass info lines (font selections) per file */

/* libass levels: 0 fatal, 1 error, 2 warning, 4 info (the font-provider
 * banner and every "fontselect: (family) -> file" pick — field receipts
 * for WHICH font drew a sign), 5+ verbose. Two caps so a chatty info
 * stream can never eat the warning budget (host harness 2026-09-08). */
static int s_ass_info_count = 0;

static void sub_ass_msg_cb(int level, const char *fmt, va_list va, void *data) {
    PlayerState *ps = (PlayerState *)data;
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, va);
    if (level >= 5) {                   /* verbose: debug builds only */
        sub_vlog("libass[%d]: %s", level, buf);
        return;
    }
    if (level >= 3) {                   /* info */
        if (s_ass_info_count >= SUB_ASS_INFO_CAP) return;
        if (++s_ass_info_count == SUB_ASS_INFO_CAP) {
            log_msg("libass: further info lines suppressed for this file (%d logged)",
                    SUB_ASS_INFO_CAP);
            return;
        }
        log_msg("libass: %s", buf);
        return;
    }
    if (!ps || ps->ass_msg_count >= SUB_ASS_MSG_CAP) return;
    if (++ps->ass_msg_count == SUB_ASS_MSG_CAP) {
        log_msg("libass: further warnings suppressed for this file (%d logged)",
                SUB_ASS_MSG_CAP);
        return;
    }
    log_msg("libass %s: %s", level <= 1 ? "ERROR" : "warning", buf);
}

static const char *sub_ass_provider_name(ASS_DefaultFontProvider p) {
    switch (p) {
        case ASS_FONTPROVIDER_NONE:        return "none";
        case ASS_FONTPROVIDER_AUTODETECT:  return "autodetect";
        case ASS_FONTPROVIDER_CORETEXT:    return "CoreText";
        case ASS_FONTPROVIDER_FONTCONFIG:  return "fontconfig";
        case ASS_FONTPROVIDER_DIRECTWRITE: return "DirectWrite";
        default:                           return "unknown";
    }
}

/* Is this attachment stream a font? Matroska attachments carry a
 * mimetype; FFmpeg also types the common ones (TTF/OTF). */
static int sub_ass_attachment_is_font(const AVStream *st) {
    const AVCodecParameters *cp = st->codecpar;
    if (cp->codec_type != AVMEDIA_TYPE_ATTACHMENT) return 0;
    if (!cp->extradata || cp->extradata_size <= 0) return 0;
    if (cp->codec_id == AV_CODEC_ID_TTF || cp->codec_id == AV_CODEC_ID_OTF) return 1;
    const AVDictionaryEntry *mime = av_dict_get(st->metadata, "mimetype", NULL, 0);
    if (!mime || !mime->value) return 0;
    const char *m = mime->value;
    return strncmp(m, "font/", 5) == 0 ||
           strstr(m, "truetype") != NULL ||
           strstr(m, "opentype") != NULL ||
           strstr(m, "font-sfnt") != NULL ||
           strstr(m, "x-font") != NULL;
}

/* Per-file library + renderer. Idempotent. */
static int sub_ass_file_init(PlayerState *ps) {
    if (ps->ass_lib && ps->ass_rend) return 0;
    double t0 = get_time_sec();

    ps->ass_msg_count      = 0;
    s_ass_info_count       = 0;
    ps->ass_fonts_embedded = 0;
    ps->ass_frame_w = ps->ass_frame_h = 0;
    ps->ass_stor_w  = ps->ass_stor_h  = 0;

    ps->ass_lib = ass_library_init();
    if (!ps->ass_lib) {
        log_msg("Subs: libass library init failed");
        return -1;
    }
    ass_set_message_cb(ps->ass_lib, sub_ass_msg_cb, ps);
    ass_set_extract_fonts(ps->ass_lib, 1);   /* [Fonts] sections inside the script too */

    /* Embedded fonts: the container's font attachments. libass copies
     * the data. A typeset release ships its fonts this way; without
     * them the provider substitutes and the sign no longer matches. */
    if (ps->fmt_ctx) {
        for (unsigned i = 0; i < ps->fmt_ctx->nb_streams; i++) {
            AVStream *st = ps->fmt_ctx->streams[i];
            if (!sub_ass_attachment_is_font(st)) continue;
            const AVDictionaryEntry *fn = av_dict_get(st->metadata, "filename", NULL, 0);
            ass_add_font(ps->ass_lib, (fn && fn->value) ? fn->value : "embedded",
                         (const char *)st->codecpar->extradata,
                         st->codecpar->extradata_size);
            ps->ass_fonts_embedded++;
        }
    }

    ps->ass_rend = ass_renderer_init(ps->ass_lib);
    if (!ps->ass_rend) {
        log_msg("Subs: libass renderer init failed");
        ass_library_done(ps->ass_lib);
        ps->ass_lib = NULL;
        return -1;
    }
    /* Provider: AUTODETECT = CoreText > DirectWrite > fontconfig, first
     * available. default_font is the belt-and-braces file used when the
     * provider has nothing (same finder as the SDL_ttf path). Hinting
     * NONE per the libass header: any hinting fights smooth scaling,
     * i.e. animations and precise positioning — the point of this. */
    const char *dflt = find_system_font();
    ass_set_fonts(ps->ass_rend, dflt, "sans-serif",
                  ASS_FONTPROVIDER_AUTODETECT, NULL, 1);
    ass_set_hinting(ps->ass_rend, ASS_HINTING_NONE);
    ass_set_shaper(ps->ass_rend, ASS_SHAPING_COMPLEX);

    /* The list always opens with NONE and AUTODETECT; the real providers
     * follow (harness receipt: list[0] read "none" on a fontconfig box). */
    const char *prov = "none";
    {
        ASS_DefaultFontProvider *list = NULL;
        size_t n = 0;
        ass_get_available_font_providers(ps->ass_lib, &list, &n);
        if (list && n != (size_t)-1) {
            for (size_t i = 0; i < n; i++) {
                if (list[i] > ASS_FONTPROVIDER_AUTODETECT) { prov = sub_ass_provider_name(list[i]); break; }
            }
        }
        free(list);
    }
    log_msg("Subs: libass 0x%08x ready in %.0f ms — font provider %s, "
            "%d embedded font(s), default font %s",
            ass_library_version(), (get_time_sec() - t0) * 1000.0,
            prov, ps->ass_fonts_embedded, dflt ? dflt : "none");
    return 0;
}

static void sub_ass_track_close(PlayerState *ps) {
    if (ps->ass_track) {
        ass_free_track(ps->ass_track);
        ps->ass_track = NULL;
    }
    ps->sub_ass_active = 0;
}

/* Open a libass track for the codec just opened in sub_open_codec.
 * The codec context stays open too: it owns the extradata and the
 * seek handler flushes it; packets bypass avcodec_decode_subtitle2
 * (for AV_CODEC_ID_ASS the decoder's rect->ass IS the packet). */
static int sub_ass_track_open(PlayerState *ps) {
    if (sub_ass_file_init(ps) < 0) return -1;
    sub_ass_track_close(ps);
    ps->ass_track = ass_new_track(ps->ass_lib);
    if (!ps->ass_track) {
        log_msg("Subs: libass track allocation failed");
        return -1;
    }
    AVCodecContext *cc = ps->sub_codec_ctx;
    if (cc && cc->extradata && cc->extradata_size > 0)
        ass_process_codec_private(ps->ass_track, (const char *)cc->extradata,
                                  cc->extradata_size);
    else
        log_msg("Subs: ASS track carries no script header — libass defaults apply");
    ps->ass_nopts_logged = 0;
    ps->sub_ass_active   = 1;
    /* libass allocates its own style slot 0 before the script's styles
     * (an empty track already has n_styles == 1), so the authored count
     * is n_styles - 1 — the number the script's author wrote. */
    const char *clk_env = SDL_getenv("DSVP_ASS_AUDIO_CLOCK");
    const char *clk = (ps->video_stream_idx < 0) ? "audio (no video stream)"
                    : (clk_env && clk_env[0] == '1') ? "audio (DSVP_ASS_AUDIO_CLOCK)"
                    : "frame PTS";
    log_msg("Subs: ASS via libass — %s script, PlayRes %dx%d, %d authored style(s), "
            "YCbCr header %d, fonts: %d embedded, render clock: %s",
            ps->ass_track->track_type == TRACK_TYPE_SSA ? "SSA" : "ASS",
            ps->ass_track->PlayResX, ps->ass_track->PlayResY,
            ps->ass_track->n_styles > 0 ? ps->ass_track->n_styles - 1 : 0,
            (int)ps->ass_track->YCbCrMatrix, ps->ass_fonts_embedded, clk);
    return 0;
}

/* Feed every queued event to libass. Not due-gated like the SDL_ttf
 * cues: libass owns timing, and an event fed early renders at its
 * own time (\move and \t need the whole event up front). */
static void sub_ass_drain(PlayerState *ps, PacketQueue *spq, const AVStream *st) {
    double tb = av_q2d(st->time_base);
    for (;;) {
        AVPacket pkt;
        if (pq_get(spq, &pkt, 0) <= 0) break;
        if (pkt.pts == AV_NOPTS_VALUE || pkt.duration <= 0 || pkt.size <= 0 || !pkt.data) {
            if (!ps->ass_nopts_logged) {
                ps->ass_nopts_logged = 1;
                log_msg("Subs: ASS event without PTS/duration dropped (once per file: pts=%s dur=%lld size=%d)",
                        pkt.pts == AV_NOPTS_VALUE ? "none" : "set",
                        (long long)pkt.duration, pkt.size);
            }
            av_packet_unref(&pkt);
            continue;
        }
        long long t_ms = (long long)llround((double)pkt.pts * tb * 1000.0);
        long long d_ms = (long long)llround((double)pkt.duration * tb * 1000.0);
        if (d_ms <= 0) d_ms = 1;
        ass_process_chunk(ps->ass_track, (const char *)pkt.data, pkt.size, t_ms, d_ms);
        av_packet_unref(&pkt);
    }
    /* The overlay gates on sub_valid + the PTS window; libass owns the
     * timing, so the window is the whole file. The seek handler drops
     * sub_valid on the demux thread; this re-arms it every drain. */
    ps->sub_is_bitmap = 0;
    ps->sub_start_pts = -1.0e9;
    ps->sub_end_pts   =  1.0e12;
    ps->sub_valid     = 1;
}

/* Render the active ASS track for one frame. frame = the video's
 * output rectangle in physical pixels; images come back positioned
 * inside it. Storage = the source picture, so PlayRes/blur/\org map
 * with the right pixel aspect (libass derives it from the pair). */
ASS_Image *sub_ass_render(PlayerState *ps, int frame_w, int frame_h,
                          double now_sec, int *changed) {
    if (!ps->sub_ass_active || !ps->ass_rend || !ps->ass_track) return NULL;
    if (frame_w <= 0 || frame_h <= 0) return NULL;
    if (ps->vid_w > 0 && ps->vid_h > 0 &&
        (ps->vid_w != ps->ass_stor_w || ps->vid_h != ps->ass_stor_h)) {
        ass_set_storage_size(ps->ass_rend, ps->vid_w, ps->vid_h);
        ps->ass_stor_w = ps->vid_w;
        ps->ass_stor_h = ps->vid_h;
    }
    if (frame_w != ps->ass_frame_w || frame_h != ps->ass_frame_h) {
        ass_set_frame_size(ps->ass_rend, frame_w, frame_h);
        ps->ass_frame_w = frame_w;
        ps->ass_frame_h = frame_h;
    }
    long long now_ms = (long long)llround(now_sec * 1000.0);
    if (now_ms < 0) now_ms = 0;
    return ass_render_frame(ps->ass_rend, ps->ass_track, now_ms, changed);
}

void sub_ass_close_file(PlayerState *ps) {
    sub_ass_track_close(ps);
    if (ps->ass_rend) { ass_renderer_done(ps->ass_rend); ps->ass_rend = NULL; }
    if (ps->ass_lib)  { ass_library_done(ps->ass_lib);   ps->ass_lib  = NULL; }
    ps->ass_frame_w = ps->ass_frame_h = 0;
    ps->ass_stor_w  = ps->ass_stor_h  = 0;
}

#else  /* !DSVP_HAVE_LIBASS */

static void sub_ass_track_close(PlayerState *ps) { ps->sub_ass_active = 0; }
void sub_ass_close_file(PlayerState *ps) { ps->sub_ass_active = 0; }

#endif /* DSVP_HAVE_LIBASS */


/* ═══════════════════════════════════════════════════════════════════
 * Stream Discovery
 * ═══════════════════════════════════════════════════════════════════ */

void sub_find_streams(PlayerState *ps) {
    ps->sub_count      = 0;
    ps->sub_selection  = 0;
    ps->sub_active_idx = -1;

    int skipped_over_cap = 0;
    for (unsigned i = 0; i < ps->fmt_ctx->nb_streams; i++) {
        AVStream *st = ps->fmt_ctx->streams[i];
        if (st->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) continue;
        if (ps->sub_count >= MAX_SUB_STREAMS) { skipped_over_cap++; continue; }

        enum AVCodecID cid = st->codecpar->codec_id;

        /* Check if this is a supported text subtitle */
        int is_text = (cid == AV_CODEC_ID_SRT ||
                       cid == AV_CODEC_ID_SUBRIP ||
                       cid == AV_CODEC_ID_ASS ||
                       cid == AV_CODEC_ID_SSA ||
                       cid == AV_CODEC_ID_MOV_TEXT ||
                       cid == AV_CODEC_ID_TEXT ||
                       cid == AV_CODEC_ID_WEBVTT);

        /* Check if this is a supported bitmap subtitle */
        int is_bitmap = (cid == AV_CODEC_ID_HDMV_PGS_SUBTITLE ||
                         cid == AV_CODEC_ID_DVD_SUBTITLE ||
                         cid == AV_CODEC_ID_DVB_SUBTITLE);

        if (!is_text && !is_bitmap) {
            log_msg("Subtitle stream %d: skipping unsupported codec %s", i,
                avcodec_get_name(cid));
            continue;
        }

        int idx = ps->sub_count;
        ps->sub_stream_indices[idx] = (int)i;

        const AVDictionaryEntry *lang  = av_dict_get(st->metadata, "language", NULL, 0);
        const AVDictionaryEntry *title = av_dict_get(st->metadata, "title", NULL, 0);

        if (title && lang) {
            snprintf(ps->sub_stream_names[idx], sizeof(ps->sub_stream_names[idx]),
                "%s (%s)", title->value, lang->value);
        } else if (lang) {
            snprintf(ps->sub_stream_names[idx], sizeof(ps->sub_stream_names[idx]),
                "%s", lang->value);
        } else if (title) {
            snprintf(ps->sub_stream_names[idx], sizeof(ps->sub_stream_names[idx]),
                "%s", title->value);
        } else {
            snprintf(ps->sub_stream_names[idx], sizeof(ps->sub_stream_names[idx]),
                "Track %d", idx + 1);
        }

        log_msg("Subtitle stream %d: [%d] %s (%s)", idx, (int)i,
            ps->sub_stream_names[idx], avcodec_get_name(cid));
        ps->sub_count++;
    }

    log_msg("Found %d subtitle stream(s) (text and bitmap)", ps->sub_count);
    if (skipped_over_cap)
        log_msg("Sub: %d more subtitle stream(s) beyond the %d-track catalogue — "
                "not selectable (m-S3-i)", skipped_over_cap, MAX_SUB_STREAMS);
}


/* ═══════════════════════════════════════════════════════════════════
 * Codec Open / Close
 * ═══════════════════════════════════════════════════════════════════ */

int sub_open_codec(PlayerState *ps, int stream_idx) {
    sub_close_codec(ps);

    if (stream_idx < 0) return 0;

    AVStream *st = ps->fmt_ctx->streams[stream_idx];
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) {
        log_msg("ERROR: No decoder for subtitle codec %s",
            avcodec_get_name(st->codecpar->codec_id));
        return -1;
    }

    ps->sub_codec_ctx = avcodec_alloc_context3(codec);
    if (!ps->sub_codec_ctx) {
        log_msg("Sub: codec context allocation failed");
        return -1;
    }
    if (avcodec_parameters_to_context(ps->sub_codec_ctx, st->codecpar) < 0) {
        log_msg("Sub: codec parameter copy failed");
        avcodec_free_context(&ps->sub_codec_ctx);
        return -1;
    }

    int ret = avcodec_open2(ps->sub_codec_ctx, codec, NULL);
    if (ret < 0) {
        log_msg("ERROR: Cannot open subtitle codec: %s", av_err2str(ret));
        avcodec_free_context(&ps->sub_codec_ctx);
        return -1;
    }

    ps->sub_active_idx = stream_idx;
    log_msg("Subtitle codec opened: %s (stream %d), canvas %dx%d",
        codec->name, stream_idx,
        ps->sub_codec_ctx->width, ps->sub_codec_ctx->height);

    /* Diagnostic: log codec extradata for PGS format analysis */
    if (ps->sub_codec_ctx->extradata_size > 0) {
        char hex[128] = {0};
        int dump_len = ps->sub_codec_ctx->extradata_size < 20
                      ? ps->sub_codec_ctx->extradata_size : 20;
        for (int i = 0; i < dump_len; i++)
            snprintf(hex + i * 3, sizeof(hex) - i * 3, "%02X ",
                     ps->sub_codec_ctx->extradata[i]);
        log_msg("Subtitle extradata (%d bytes): %s",
                ps->sub_codec_ctx->extradata_size, hex);
    } else {
        log_msg("Subtitle extradata: none");
    }

    /* ASS/SSA → libass when built with it and not switched off; every
     * other outcome names itself so a log never leaves the path in doubt. */
    enum AVCodecID cid = st->codecpar->codec_id;
    if (cid == AV_CODEC_ID_ASS || cid == AV_CODEC_ID_SSA) {
#ifdef DSVP_HAVE_LIBASS
        if (SDL_getenv("DSVP_NO_LIBASS"))
            log_msg("Subs: libass disabled (DSVP_NO_LIBASS) — ASS override tags stripped, house style");
        else if (sub_ass_track_open(ps) < 0)
            log_msg("Subs: libass track open failed — ASS override tags stripped, house style");
#else
        log_msg("Subs: this build has no libass — ASS override tags stripped, house style "
                "(build with libass-dev / mingw-w64-x86_64-libass)");
#endif
    }
    return 0;
}

void sub_close_codec(PlayerState *ps) {
    sub_ass_track_close(ps);   /* before the codec: the track was built from its extradata */
    if (ps->sub_codec_ctx) {
        avcodec_free_context(&ps->sub_codec_ctx);
    }
    ps->sub_active_idx = -1;
    ps->sub_is_bitmap = 0;
    sub_clear_display(ps);
}


/* ═══════════════════════════════════════════════════════════════════
 * Track Cycling
 * ═══════════════════════════════════════════════════════════════════
 *
 * No seeking is performed — subtitles appear from the next event
 * in the container. This is standard behavior (VLC, mpv do the same).
 */

void sub_cycle(PlayerState *ps) {
    if (ps->sub_count == 0) {
        snprintf(ps->sub_osd, sizeof(ps->sub_osd), "No subtitles available");
        ps->sub_osd_until = get_time_sec() + 2.0;
        return;
    }

    /* Cycle: 0 (off) → 1 → 2 → ... → N → 0 (off).
     * Queues are NOT flushed here: the demuxer keeps every track as a
     * rolling ~35s window precisely so the newly selected track has the
     * current moment's packets on hand — flushing made S appear dead
     * for the ~10s it took playback to reach the demux read position.
     * The decode-side stale-skip absorbs the (bounded) backlog. */
    ps->sub_selection = (ps->sub_selection + 1) % (ps->sub_count + 1);

    if (ps->sub_selection == 0) {
        /* Codec teardown under seek_mutex: the demux seek handler
         * NULL-checks-then-flushes sub_codec_ctx — freeing it here
         * unlocked is a use-after-free window (S during a seek). */
        SDL_LockMutex(ps->seek_mutex);
        sub_close_codec(ps);
        SDL_UnlockMutex(ps->seek_mutex);
        snprintf(ps->sub_osd, sizeof(ps->sub_osd), "Subtitles: Off");
        log_msg("Subtitles disabled");
    } else {
        int sel = ps->sub_selection - 1;
        int stream_idx = ps->sub_stream_indices[sel];

        SDL_LockMutex(ps->seek_mutex);
        int open_ret = sub_open_codec(ps, stream_idx);
        SDL_UnlockMutex(ps->seek_mutex);

        /* Clear current display so new track takes effect immediately */
        ps->sub_is_bitmap = 0;
        sub_clear_display(ps);

        if (open_ret < 0) {
            /* Announcing the track anyway would leave sub_selection
             * pointing at a dead codec and the OSD lying about it. */
            snprintf(ps->sub_osd, sizeof(ps->sub_osd),
                "Subtitles: %s (codec error)", ps->sub_stream_names[sel]);
            log_msg("Subtitles: failed to open %s (stream %d)",
                ps->sub_stream_names[sel], stream_idx);
        } else {
            snprintf(ps->sub_osd, sizeof(ps->sub_osd), "Subtitles: %s",
                ps->sub_stream_names[sel]);
            log_msg("Subtitles: %s (stream %d)",
                ps->sub_stream_names[sel], stream_idx);
        }
    }

    ps->sub_osd_until = get_time_sec() + 2.0;
}


/* ═══════════════════════════════════════════════════════════════════
 * ASS Markup Stripping
 * ═══════════════════════════════════════════════════════════════════ */

/* Byte-bounded copies can cut a multibyte UTF-8 sequence at the buffer
 * boundary; SDL3_ttf then renders U+FFFD (or fails the line) for the
 * tail. Walk back over any trailing partial sequence and drop it. */
static void utf8_trim_partial(char *s) {
    size_t len = strlen(s);
    size_t i = len;
    /* Back over up to 3 continuation bytes (10xxxxxx) */
    while (i > 0 && ((unsigned char)s[i - 1] & 0xC0) == 0x80 && len - i < 3)
        i--;
    if (i == 0) return;
    unsigned char lead = (unsigned char)s[i - 1];
    size_t need = 0;
    if      ((lead & 0x80) == 0x00) need = 1;
    else if ((lead & 0xE0) == 0xC0) need = 2;
    else if ((lead & 0xF0) == 0xE0) need = 3;
    else if ((lead & 0xF8) == 0xF0) need = 4;
    else { s[i - 1] = '\0'; return; }   /* stray continuation byte */
    if (len - (i - 1) < need)
        s[i - 1] = '\0';                /* sequence incomplete — drop it */
}

static void strip_ass_markup(const char *ass_event, char *out, int out_size) {
    const char *p = ass_event;
    int commas = 0;
    while (*p && commas < 8) {
        if (*p == ',') commas++;
        p++;
    }

    if (commas < 8) p = ass_event;

    int o = 0;
    while (*p && o < out_size - 1) {
        if (*p == '{') {
            while (*p && *p != '}') p++;
            if (*p == '}') p++;
            continue;
        }
        if (*p == '\\' && (*(p + 1) == 'N' || *(p + 1) == 'n')) {
            if (o < out_size - 1) out[o++] = '\n';
            p += 2;
            continue;
        }
        if (*p == '\\' && *(p + 1) == 'h') {   /* ASS hard space (m-S3-e) */
            out[o++] = ' ';
            p += 2;
            continue;
        }
        out[o++] = *p++;
    }
    out[o] = '\0';

    while (o > 0 && (out[o - 1] == ' ' || out[o - 1] == '\n' || out[o - 1] == '\r')) {
        out[--o] = '\0';
    }
    char *start = out;
    while (*start == ' ' || *start == '\n' || *start == '\r') start++;
    if (start != out) memmove(out, start, strlen(start) + 1);

    /* The byte-bounded copy above can end mid-codepoint on a >buffer
     * event — drop any trailing partial UTF-8 sequence. */
    utf8_trim_partial(out);
}


/* ═══════════════════════════════════════════════════════════════════
 * PGS Zlib Decompression
 * ═══════════════════════════════════════════════════════════════════
 *
 * Some MKV muxers apply ContentCompression (zlib) to PGS subtitle
 * tracks. FFmpeg's matroska demuxer doesn't always decompress these
 * transparently, leaving raw zlib data in the AVPacket. Detect via
 * the 0x78 zlib magic byte and decompress before decoding.
 *
 * Returns: newly allocated decompressed buffer (caller must av_free),
 *          or NULL if not compressed / decompression failed.
 *          *out_size is set to the decompressed length on success.
 */
static uint8_t *pgs_try_decompress(const uint8_t *data, int size, int *out_size) {
    if (size < 2 || data[0] != 0x78) return NULL;
    /* 0x78 followed by 0x01/0x5E/0x9C/0xDA = valid zlib header */
    uint8_t flg = data[1];
    if (flg != 0x01 && flg != 0x5E && flg != 0x9C && flg != 0xDA)
        return NULL;

    /* Start with 10x buffer, retry with larger if needed */
    uLongf dst_len = (uLongf)size * 10;
    for (int attempt = 0; attempt < 3; attempt++) {
        uint8_t *dst = av_malloc(dst_len);
        if (!dst) return NULL;

        int zret = uncompress(dst, &dst_len, data, (uLong)size);
        if (zret == Z_OK) {
            *out_size = (int)dst_len;
            return dst;
        }
        av_free(dst);
        if (zret == Z_BUF_ERROR) {
            dst_len *= 4;  /* buffer too small, try larger */
            continue;
        }
        /* Z_DATA_ERROR or other — not valid zlib */
        return NULL;
    }
    return NULL;
}


/* ═══════════════════════════════════════════════════════════════════
 * Subtitle Decoding
 * ═══════════════════════════════════════════════════════════════════
 *
 * Called from the main thread each frame. Pops ONE subtitle at a
 * time from the queue and holds it until its display time expires.
 * Skips subtitles whose end time has already passed.
 */

static void sub_decode_pending_impl(PlayerState *ps);

void sub_decode_pending(PlayerState *ps) {
    if (ps->sub_active_idx < 0 || !ps->sub_codec_ctx) return;
    /* The demux seek handler flushes sub_codec_ctx under seek_mutex, and
     * concurrent flush-vs-decode on one AVCodecContext is UB (for PGS the
     * flush clears segment state the decoder is mutating mid-call). Same
     * try-lock convention as the video decode thread: if a seek holds the
     * mutex, skip this frame — the flush is about to invalidate the queue
     * we would have drained anyway. */
    if (!SDL_TryLockMutex(ps->seek_mutex)) return;
    sub_decode_pending_impl(ps);
    SDL_UnlockMutex(ps->seek_mutex);
}

/* ═══════════════════════════════════════════════════════════════════
 * Batch 7 (review 2026-09, subtitle batch): the decode state machine
 * was rebuilt around two rules that the old one broke —
 *   TEXT: several cues can be on screen at once (M4). Each has its
 *         own window; the set is joined for the overlay; a cue is
 *         popped when DUE (its start has come) and dropped when its
 *         end has passed, independently of the others.
 *   BITMAP: never pop a display set whose time has not come, whether
 *         or not something is displayed (M5 — the old code drained
 *         the whole queue while nothing showed and lost every
 *         intermediate PGS set on END-stripped MKV), inject the
 *         synthetic END at display-set BOUNDARIES (the next packet
 *         has a different PTS) instead of once per drain, and while a
 *         bitmap is displayed always drain due packets (M6 — the 29 s
 *         heuristic only ran the drain when the end came from the cap).
 *         Among several due sets consumed in one call the last state
 *         wins (m-S3-a: the S-press burst of stale captions).
 * ═══════════════════════════════════════════════════════════════════ */

#define SUB_BITMAP_CAP_SEC   120.0  /* memory bound on a bitmap set whose
                                     * clear never arrives (m-S3-c: was
                                     * 30 s and hid long captions)      */
#define SUB_STALE_SKIP_SEC    30.0  /* bitmap packets older than this are
                                     * not decoded on catch-up (S-press
                                     * cycling pulled every set from
                                     * file start through now: 312 drops
                                     * on a 4K DV title). Deliberately
                                     * shorter than the cap: a set older
                                     * than 30 s is almost never the one
                                     * meant to be on screen, and the
                                     * cost is the point.               */

static int sub_track_is_text(const PlayerState *ps) {
    const AVCodecDescriptor *d = avcodec_descriptor_get(ps->sub_codec_ctx->codec_id);
    return d && (d->props & AV_CODEC_PROP_TEXT_SUB);
}

/* Rebuild the joined render string from the active set (sorted by
 * start so a later cue reads below an earlier one). */
static void sub_cues_rebuild(PlayerState *ps) {
    ps->sub_text[0] = '\0';
    if (ps->sub_cue_count <= 0) {
        ps->sub_cue_count = 0;
        ps->sub_valid = 0;
        return;
    }
    /* insertion sort by start — the set is tiny */
    for (int i = 1; i < ps->sub_cue_count; i++) {
        for (int j = i; j > 0 && ps->sub_cues[j - 1].start > ps->sub_cues[j].start; j--) {
            SubCue t = ps->sub_cues[j - 1];
            ps->sub_cues[j - 1] = ps->sub_cues[j];
            ps->sub_cues[j] = t;
        }
    }
    size_t used = 0;
    double lo = ps->sub_cues[0].start, hi = ps->sub_cues[0].end;
    for (int i = 0; i < ps->sub_cue_count; i++) {
        if (used > 0 && used < sizeof(ps->sub_text) - 1)
            ps->sub_text[used++] = '\n';
        int n = snprintf(ps->sub_text + used, sizeof(ps->sub_text) - used,
                         "%s", ps->sub_cues[i].text);
        if (n < 0) break;
        used += (size_t)n;
        if (used >= sizeof(ps->sub_text) - 1) { used = sizeof(ps->sub_text) - 1; break; }
        if (ps->sub_cues[i].start < lo) lo = ps->sub_cues[i].start;
        if (ps->sub_cues[i].end   > hi) hi = ps->sub_cues[i].end;
    }
    ps->sub_text[used] = '\0';
    utf8_trim_partial(ps->sub_text);
    ps->sub_is_bitmap = 0;
    ps->sub_start_pts = lo;
    ps->sub_end_pts   = hi;
    ps->sub_valid     = 1;
}

/* Drop cues whose end has passed. Returns how many. */
static int sub_cues_expire(PlayerState *ps, double now) {
    int dropped = 0;
    for (int i = 0; i < ps->sub_cue_count; ) {
        if (ps->sub_cues[i].end < now) {
            sub_vlog("Sub: cue expired (start=%.2f end=%.2f < now=%.2f)",
                     ps->sub_cues[i].start, ps->sub_cues[i].end, now);
            ps->sub_cues[i] = ps->sub_cues[ps->sub_cue_count - 1];
            ps->sub_cue_count--;
            dropped++;
        } else {
            i++;
        }
    }
    if (dropped) sub_cues_rebuild(ps);
    return dropped;
}

static void sub_cue_add(PlayerState *ps, const char *text, double start, double end) {
    if (ps->sub_cue_count >= SUB_MAX_ACTIVE_CUES) {
        /* Bounded set: the cue ending soonest makes room. */
        int victim = 0;
        for (int i = 1; i < ps->sub_cue_count; i++)
            if (ps->sub_cues[i].end < ps->sub_cues[victim].end) victim = i;
        log_msg("Sub: %d cues active — dropping the one ending at %.2f to add one",
                ps->sub_cue_count, ps->sub_cues[victim].end);
        ps->sub_cues[victim] = ps->sub_cues[ps->sub_cue_count - 1];
        ps->sub_cue_count--;
    }
    int k = ps->sub_cue_count++;
    snprintf(ps->sub_cues[k].text, sizeof(ps->sub_cues[k].text), "%s", text);
    utf8_trim_partial(ps->sub_cues[k].text);
    ps->sub_cues[k].start = start;
    ps->sub_cues[k].end   = end;
    if (k > 0)   /* engage line for the field leg (M4) */
        log_msg("Sub: overlapping cue added (start=%.2f end=%.2f, %d active)",
                start, end, ps->sub_cue_count);
    else
        sub_vlog("Sub: cue shown (start=%.2f end=%.2f)", start, end);
    sub_cues_rebuild(ps);
}

/* Paletted rects → RGBA into the display slots. Replaces whatever was
 * displayed. Returns 1 if at least one rect landed. One converter for
 * the main loop and the END inject (they had drifted: the inject copy
 * lacked the duration fallback and the rect-type log). */
static int sub_commit_bitmap_set(PlayerState *ps, const AVSubtitle *sub,
                                 double start, double end, const char *tag) {
    static int s_palette_clamp_logged = 0;
    (void)tag;   /* only the verbose (DSVP_DEBUG) lines print it */
    sub_clear_bitmaps(ps);
    int got_bitmap = 0;
    for (unsigned i = 0; i < sub->num_rects; i++) {
        const AVSubtitleRect *rect = sub->rects[i];
        if (rect->type != SUBTITLE_BITMAP || !rect->data[0] || !rect->data[1] ||
            rect->w <= 0 || rect->h <= 0)
            continue;
        if (ps->sub_bitmap_count >= MAX_SUB_BITMAPS) {
            sub_vlog("Sub: rect %u dropped — %d bitmap slots", i, MAX_SUB_BITMAPS);
            continue;
        }
        /* rect->data[0] = palette indices, data[1] = 0xAARRGGBB palette,
         * rect->x/y = position in the video frame (typeset PGS signs
         * land where the disc drew them). nb_colors bounds the palette
         * read (m-S3-b): out-of-range indices are transparent. */
        const uint32_t *palette = (const uint32_t *)rect->data[1];
        int ncol = rect->nb_colors > 0 && rect->nb_colors <= 256 ? rect->nb_colors : 256;
        int w = rect->w, h = rect->h;
        uint8_t *rgba = ((int64_t)w * h > (int64_t)INT_MAX / 4)
                        ? NULL : av_malloc((size_t)w * h * 4);   /* overflow guard */
        if (!rgba) continue;
        int clamped = 0;
        for (int row = 0; row < h; row++) {
            const uint8_t *src = rect->data[0] + (size_t)row * rect->linesize[0];
            uint8_t *dst = rgba + (size_t)row * w * 4;
            for (int col = 0; col < w; col++) {
                uint8_t idx = src[col];
                uint32_t color = 0;
                if (idx < ncol) color = palette[idx]; else clamped++;
                dst[col * 4 + 0] = (color >> 16) & 0xFF;  /* R */
                dst[col * 4 + 1] = (color >> 8)  & 0xFF;  /* G */
                dst[col * 4 + 2] =  color        & 0xFF;  /* B */
                dst[col * 4 + 3] = (color >> 24) & 0xFF;  /* A */
            }
        }
        if (clamped && !s_palette_clamp_logged) {
            s_palette_clamp_logged = 1;
            log_msg("Sub: %d palette index(es) >= nb_colors %d rendered transparent "
                    "(once per session, m-S3-b)", clamped, ncol);
        }
        int bi = ps->sub_bitmap_count;
        ps->sub_bitmap_data[bi]  = rgba;   /* ownership transferred */
        ps->sub_bitmap_w[bi]     = w;
        ps->sub_bitmap_h[bi]     = h;
        ps->sub_bitmap_rects[bi] = (SDL_Rect){ rect->x, rect->y, w, h };
        ps->sub_bitmap_count++;
        got_bitmap = 1;
        sub_vlog("Sub [%s] %.1f-%.1f: %dx%d at (%d,%d)", tag, start, end, w, h, rect->x, rect->y);
    }
    if (got_bitmap) {
        ps->sub_cue_count = 0;
        ps->sub_is_bitmap = 1;
        ps->sub_text[0]   = '\0';
        ps->sub_start_pts = start;
        ps->sub_end_pts   = end;
        ps->sub_valid     = 1;
    }
    return got_bitmap;
}

/* Bitmap timing from a decoded AVSubtitle: start/end relative to the
 * packet PTS, the subrip-style pkt.duration fallback, then the memory
 * cap (m-S3-c). */
static void sub_bitmap_window(const AVSubtitle *sub, double pkt_pts, double dur_sec,
                              double *start, double *end) {
    *start = pkt_pts + (double)sub->start_display_time / 1000.0;
    *end   = pkt_pts + (double)sub->end_display_time   / 1000.0;
    if (sub->end_display_time == 0 && dur_sec > 0.0)      *end = pkt_pts + dur_sec;
    else if (sub->end_display_time == 0)                  *end = *start + 3.0;
    if (*end - *start > SUB_BITMAP_CAP_SEC)               *end = *start + SUB_BITMAP_CAP_SEC;
}

/* Hand a decoded display set to the screen if it is current. Returns
 * 1 if displayed, 0 if it was already over (superseded) or empty. */
static int sub_bitmap_present(PlayerState *ps, AVSubtitle *sub, double pkt_pts,
                              double start, double end, double now, const char *tag) {
    if (sub->num_rects == 0) {
        /* The clear signal. Due-only popping means a clear in the
         * future only reaches here without a PTS. */
        sub_vlog("Sub: clear signal (0 rects, pts=%.1f)", pkt_pts);
        if (pkt_pts > now && ps->sub_valid) { ps->sub_end_pts = pkt_pts; return 0; }
        ps->sub_valid = 0;
        sub_clear_bitmaps(ps);
        return 0;
    }
    if (end < now) {
        sub_vlog("Sub: %s set superseded (end=%.1f < now=%.1f)", tag, end, now);
        return 0;
    }
    if (ps->sub_valid && ps->sub_is_bitmap && now < ps->sub_end_pts)
        log_msg("Sub: due display set replaced caption early (was end=%.2f, new pts=%.2f)",
                ps->sub_end_pts, pkt_pts);
    return sub_commit_bitmap_set(ps, sub, start, end, tag);
}

/* Synthetic END for END-stripped PGS: pgssubdec only emits a display
 * set on DISPLAY_SEGMENT (0x80), which some MKV muxers drop. Fired at
 * a set boundary (the next packet carries a different PTS, is in the
 * future, or the queue is empty) so the set is complete and nothing
 * from the next set has been fed. */
static int pgs_inject_end(PlayerState *ps, double set_pts, double next_pts, double now,
                          int *superseded) {
    static const uint8_t end_seg[] = { 0x80, 0x00, 0x00 };
    AVPacket end_pkt;
    memset(&end_pkt, 0, sizeof(end_pkt));
    end_pkt.data = (uint8_t *)end_seg;
    end_pkt.size = sizeof(end_seg);
    AVSubtitle sub;
    int got_sub = 0;
    int ret = avcodec_decode_subtitle2(ps->sub_codec_ctx, &sub, &got_sub, &end_pkt);
    if (ret < 0 || !got_sub) {
        sub_vlog("Sub: PGS-END inject at %.1f: got_sub=%d ret=%d", set_pts, got_sub, ret);
        return 0;
    }
    log_msg("Sub: PGS-END injected at set boundary (set pts=%.2f, next pts=%.2f, rects=%u)",
            set_pts, next_pts, sub.num_rects);
    double start, end;
    sub_bitmap_window(&sub, set_pts, 0.0, &start, &end);
    int shown = sub_bitmap_present(ps, &sub, set_pts, start, end, now, "PGS BITMAP");
    if (!shown && sub.num_rects > 0 && end < now) (*superseded)++;
    avsubtitle_free(&sub);
    return shown;
}

/* Time of the packet at the head of the queue in seconds, or -1 if
 * the queue is empty or the head has no PTS. */
static double sub_head_sec(PlayerState *ps, PacketQueue *spq) {
    int64_t head_pts;
    if (!pq_peek_pts(spq, &head_pts) || head_pts == AV_NOPTS_VALUE) return -1.0;
    AVStream *st = ps->fmt_ctx->streams[ps->sub_active_idx];
    return (double)head_pts * av_q2d(st->time_base);
}

static void sub_decode_pending_impl(PlayerState *ps) {
    if (ps->sub_active_idx < 0 || !ps->sub_codec_ctx) return;
    if (ps->sub_selection <= 0 || ps->sub_selection > ps->sub_count) return;

    int queue_idx = ps->sub_selection - 1;
    PacketQueue *spq = &ps->sub_pqs[queue_idx];
    AVStream *st = ps->fmt_ctx->streams[ps->sub_active_idx];

    double now = ps->audio_clock_sync;
    if (ps->audio_stream_idx < 0) now = ps->video_clock;

    const int text_track = sub_track_is_text(ps);
    const int is_pgs = ps->sub_codec_ctx->codec_id == AV_CODEC_ID_HDMV_PGS_SUBTITLE;

    /* ── ASS via libass: feed everything queued; libass owns timing ── */
    if (ps->sub_ass_active) {
#ifdef DSVP_HAVE_LIBASS
        if (ps->sub_is_bitmap && ps->sub_bitmap_count) sub_clear_bitmaps(ps); /* track switched */
        ps->sub_cue_count = 0;
        sub_ass_drain(ps, spq, st);
#endif
        return;
    }

    /* ── TEXT (M4): expire, then pop every DUE cue into the set ── */
    if (text_track) {
        if (ps->sub_is_bitmap && ps->sub_bitmap_count) sub_clear_bitmaps(ps); /* track switched */
        sub_cues_expire(ps, now);
        static int s_nopts_logged = 0;
        for (;;) {
            double head = sub_head_sec(ps, spq);
            if (head > now) break;              /* a future cue waits its turn */
            AVPacket pkt;
            if (pq_get(spq, &pkt, 0) <= 0) break;

            AVSubtitle sub;
            int got_sub = 0;
            int ret = avcodec_decode_subtitle2(ps->sub_codec_ctx, &sub, &got_sub, &pkt);
            if (ret < 0) {
                sub_vlog("Sub: text decode error %s", av_err2str(ret));
                av_packet_unref(&pkt);
                continue;
            }
            if (!got_sub) { av_packet_unref(&pkt); continue; }

            double start, end;
            double dur_sec = pkt.duration > 0 ? (double)pkt.duration * av_q2d(st->time_base) : 0.0;
            if (pkt.pts == AV_NOPTS_VALUE) {
                /* m-S3-h: a cue without a PTS used to be kept by the
                 * stale filter and then discarded as expired at t=0.
                 * Show it now for its duration. */
                start = now;
                end   = now + (dur_sec > 0.0 ? dur_sec : 3.0);
                if (!s_nopts_logged) {
                    s_nopts_logged = 1;
                    log_msg("Sub: cue without PTS shown now for %.1f s (once per session, m-S3-h)",
                            end - start);
                }
            } else {
                double pkt_pts = (double)pkt.pts * av_q2d(st->time_base);
                start = pkt_pts + (double)sub.start_display_time / 1000.0;
                end   = pkt_pts + (double)sub.end_display_time   / 1000.0;
                if (sub.end_display_time == 0 && dur_sec > 0.0) end = pkt_pts + dur_sec;
                else if (sub.end_display_time == 0)             end = start + 3.0;
            }

            char text[SUB_CUE_TEXT_SIZE] = {0};
            for (unsigned i = 0; i < sub.num_rects; i++) {
                const AVSubtitleRect *rect = sub.rects[i];
                const char *piece = NULL;
                char stripped[SUB_CUE_TEXT_SIZE];
                if (rect->type == SUBTITLE_TEXT && rect->text) {
                    piece = rect->text;
                } else if (rect->type == SUBTITLE_ASS && rect->ass) {
                    strip_ass_markup(rect->ass, stripped, sizeof(stripped));
                    piece = stripped;
                }
                if (!piece || !piece[0]) continue;
                size_t used = strlen(text);
                if (used > 0 && used < sizeof(text) - 1) { text[used++] = '\n'; text[used] = '\0'; }
                snprintf(text + used, sizeof(text) - used, "%s", piece);
            }
            avsubtitle_free(&sub);
            av_packet_unref(&pkt);

            if (!text[0]) continue;
            if (end < now) {
                sub_vlog("Sub: skipped expired cue (end=%.1f < now=%.1f)", end, now);
                continue;
            }
            sub_cue_add(ps, text, start, end);
        }
        return;
    }

    /* ── BITMAP (M5/M6/m-S3-a): due-only, last state wins ── */
    if (ps->sub_cue_count) { ps->sub_cue_count = 0; }   /* track switched from text */
    if (ps->sub_valid && now > ps->sub_end_pts) {
        sub_vlog("Sub: bitmap set expired (end=%.1f < now=%.1f)", ps->sub_end_pts, now);
        ps->sub_valid = 0;
        sub_clear_bitmaps(ps);
    }

    int    pgs_pending  = 0;      /* packets fed without a set out      */
    double last_pgs_pts = -1.0;   /* PTS of the set being accumulated   */
    int    superseded   = 0;      /* due sets consumed but already over */
    for (;;) {
        double head = sub_head_sec(ps, spq);
        /* Set boundary while accumulating an END-stripped set: the next
         * packet belongs to another set (different PTS) or is not due.
         * Emit the accumulated set BEFORE feeding anything else. */
        if (is_pgs && pgs_pending && last_pgs_pts >= 0.0 &&
            (head < 0.0 || head > now || head != last_pgs_pts)) {
            pgs_inject_end(ps, last_pgs_pts, head, now, &superseded);
            pgs_pending = 0;
            /* an empty queue ends the loop at the pq_get below */
        }
        if (head > now) break;                  /* never pop a future set (M5) */

        AVPacket pkt;
        if (pq_get(spq, &pkt, 0) <= 0) break;

        double pkt_pts = -1.0;
        if (pkt.pts != AV_NOPTS_VALUE) {
            pkt_pts = (double)pkt.pts * av_q2d(st->time_base);
            if (pkt_pts + SUB_STALE_SKIP_SEC < now) {
                sub_vlog("Sub: skipped stale packet (pts=%.1f + %.0f < now=%.1f)",
                         pkt_pts, SUB_STALE_SKIP_SEC, now);
                av_packet_unref(&pkt);
                continue;
            }
        }

        /* PGS zlib: some MKV muxers apply ContentCompression and the
         * demuxer does not always undo it. */
        uint8_t *decompressed = NULL;
        int decomp_size = 0;
        AVPacket decode_pkt = pkt;
        if (is_pgs) {
            decompressed = pgs_try_decompress(pkt.data, pkt.size, &decomp_size);
            if (decompressed) { decode_pkt.data = decompressed; decode_pkt.size = decomp_size; }
        }

        AVSubtitle sub;
        int got_sub = 0;
        int ret = avcodec_decode_subtitle2(ps->sub_codec_ctx, &sub, &got_sub, &decode_pkt);
        sub_vlog("Sub: MAIN-LOOP pkt_size=%d%s got_sub=%d rects=%u ret=%d seg=0x%02X",
                 pkt.size, decompressed ? " (zlib)" : "", got_sub,
                 got_sub ? sub.num_rects : 0, ret,
                 (is_pgs && decode_pkt.size > 0) ? decode_pkt.data[0] : 0);
        av_free(decompressed);

        if (is_pgs && pkt_pts >= 0.0) last_pgs_pts = pkt_pts;
        if (ret < 0) {
            sub_vlog("Sub: bitmap decode error %s", av_err2str(ret));
            av_packet_unref(&pkt);
            continue;
        }
        if (!got_sub) {
            /* Normal for PGS: segments accumulate until END. */
            if (is_pgs) pgs_pending = 1;
            av_packet_unref(&pkt);
            continue;
        }
        if (is_pgs) pgs_pending = 0;   /* the stream carried its own END */

        double base = pkt_pts >= 0.0 ? pkt_pts : now;
        double dur_sec = pkt.duration > 0 ? (double)pkt.duration * av_q2d(st->time_base) : 0.0;
        double start, end;
        sub_bitmap_window(&sub, base, dur_sec, &start, &end);
        int shown = sub_bitmap_present(ps, &sub, base, start, end, now, "BITMAP");
        if (!shown && sub.num_rects > 0 && end < now) superseded++;
        avsubtitle_free(&sub);
        av_packet_unref(&pkt);
    }
    if (is_pgs && pgs_pending && last_pgs_pts >= 0.0)
        pgs_inject_end(ps, last_pgs_pts, -1.0, now, &superseded);
    if (superseded)
        log_msg("Sub: superseded %d due display set(s) on drain", superseded);
}
