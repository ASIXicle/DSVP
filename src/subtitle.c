/*
 * DSVP — Dead Simple Video Player
 * subtitle.c — Subtitle stream detection, decoding, and rendering
 *
 * Handles:
 *   - Cataloging available subtitle tracks in a container
 *   - Opening/closing subtitle codecs
 *   - Decoding text subtitles (SRT, ASS/SSA)
 *   - Rendering with SDL_ttf: golden yellow (#FFDF00) + black outline
 *   - Track cycling with 'S' key (including "Off" option)
 */

#include "dsvp.h"
#include <zlib.h>

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


/* ═══════════════════════════════════════════════════════════════════
 * Stream Discovery
 * ═══════════════════════════════════════════════════════════════════ */

void sub_find_streams(PlayerState *ps) {
    ps->sub_count      = 0;
    ps->sub_selection  = 0;
    ps->sub_active_idx = -1;

    for (unsigned i = 0; i < ps->fmt_ctx->nb_streams && ps->sub_count < MAX_SUB_STREAMS; i++) {
        AVStream *st = ps->fmt_ctx->streams[i];
        if (st->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) continue;

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

    log_msg("Found %d text subtitle stream(s)", ps->sub_count);
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
    avcodec_parameters_to_context(ps->sub_codec_ctx, st->codecpar);

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
    return 0;
}

void sub_close_codec(PlayerState *ps) {
    if (ps->sub_codec_ctx) {
        avcodec_free_context(&ps->sub_codec_ctx);
    }
    ps->sub_active_idx = -1;
    ps->sub_valid = 0;
    ps->sub_is_bitmap = 0;
    ps->sub_text[0] = '\0';
    sub_clear_bitmaps(ps);
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

    /* Cycle: 0 (off) → 1 → 2 → ... → N → 0 (off) */
    ps->sub_selection = (ps->sub_selection + 1) % (ps->sub_count + 1);

    if (ps->sub_selection == 0) {
        sub_close_codec(ps);
        snprintf(ps->sub_osd, sizeof(ps->sub_osd), "Subtitles: Off");
        log_msg("Subtitles disabled");
    } else {
        int sel = ps->sub_selection - 1;
        int stream_idx = ps->sub_stream_indices[sel];

        sub_open_codec(ps, stream_idx);

        /* Clear current display so new track takes effect immediately */
        ps->sub_valid = 0;
        ps->sub_is_bitmap = 0;
        ps->sub_text[0] = '\0';
        sub_clear_bitmaps(ps);

        snprintf(ps->sub_osd, sizeof(ps->sub_osd), "Subtitles: %s",
            ps->sub_stream_names[sel]);
        log_msg("Subtitles: %s (stream %d)",
            ps->sub_stream_names[sel], stream_idx);
    }

    ps->sub_osd_until = get_time_sec() + 2.0;
}


/* ═══════════════════════════════════════════════════════════════════
 * ASS Markup Stripping
 * ═══════════════════════════════════════════════════════════════════ */

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
        out[o++] = *p++;
    }
    out[o] = '\0';

    while (o > 0 && (out[o - 1] == ' ' || out[o - 1] == '\n' || out[o - 1] == '\r')) {
        out[--o] = '\0';
    }
    char *start = out;
    while (*start == ' ' || *start == '\n' || *start == '\r') start++;
    if (start != out) memmove(out, start, strlen(start) + 1);
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

void sub_decode_pending(PlayerState *ps) {
    if (ps->sub_active_idx < 0 || !ps->sub_codec_ctx) return;
    if (ps->sub_selection <= 0 || ps->sub_selection > ps->sub_count) return;

    /* Get the queue for the active subtitle stream */
    int queue_idx = ps->sub_selection - 1;
    PacketQueue *spq = &ps->sub_pqs[queue_idx];

    double now = ps->audio_clock_sync;
    if (ps->audio_stream_idx < 0) now = ps->video_clock;

    /* If current subtitle is still valid and on-screen, keep it.
     * Exception: bitmap subs currently DISPLAYING need to drain the queue
     * for "clear" packets (0 rects) that signal when to hide.
     * Once the clear is found (end_pts updated from the 30s cap), stop draining. */
    if (ps->sub_valid && now <= ps->sub_end_pts) {
        if (!ps->sub_is_bitmap) return;
        if (now < ps->sub_start_pts) return;  /* not showing yet, don't drain */
        /* If end_pts was updated from the 30s cap, clear was already found */
        if (ps->sub_end_pts - ps->sub_start_pts < 29.0) return;
        /* Bitmap currently displayed, clear not yet found — drain for it */
    }

    /* Current subtitle expired or bitmap needs clear-packet drain */
    int draining_for_clear = (ps->sub_is_bitmap && ps->sub_valid
                              && now >= ps->sub_start_pts && now <= ps->sub_end_pts);
    if (!draining_for_clear) {
        ps->sub_valid = 0;
        sub_clear_bitmaps(ps);
    }

    AVPacket pkt;
    int pgs_packets_this_drain = 0;
    double last_pgs_pts = 0.0;
    while (pq_get(spq, &pkt, 0) > 0) {
        /* ── Stale-packet skip ──
         *
         * Subtitle decode is expensive — PGS bitmap especially, with zlib
         * decompression, palette/object segment accumulation, and a final
         * END-inject pass.  During subtitle-stream cycling (user tabbing
         * through tracks), opening a new codec mid-playback pulls every
         * queued packet from file-start through current playback position;
         * each catch-up decode runs on the main thread and can block for
         * tens of ms.  In one stress test (16 subtitle streams cycled
         * on a 3840x1608 4K HEVC HDR DV P8 source), this triggered 312
         * video-frame drops.
         *
         * Skip packets whose worst-case display window (pts + 30s, matching
         * the PGS display cap applied a few hundred lines below) has
         * already passed.  Same-scene PGS segment state — PCS/WDS/PDS/ODS
         * accumulation toward END — cannot span this gap, so the decoder's
         * cross-call state machine and the END-inject path below both stay
         * intact.  Packets with unknown PTS are kept (can't judge safely). */
        if (pkt.pts != AV_NOPTS_VALUE) {
            AVStream *sub_st = ps->fmt_ctx->streams[ps->sub_active_idx];
            double pkt_pts_sec = (double)pkt.pts * av_q2d(sub_st->time_base);
            if (pkt_pts_sec + 30.0 < now) {
                log_msg("Sub: skipped stale packet (pts=%.1f + 30 < now=%.1f)",
                        pkt_pts_sec, now);
                av_packet_unref(&pkt);
                continue;
            }
        }

        AVSubtitle sub;
        int got_sub = 0;

        /* PGS zlib fix: some MKV muxers apply ContentCompression (zlib)
         * to PGS tracks but FFmpeg's demuxer doesn't always decompress.
         * Detect 0x78 zlib magic and decompress before decoding. */
        uint8_t *decompressed = NULL;
        int decomp_size = 0;
        AVPacket decode_pkt = pkt;
        if (ps->sub_codec_ctx->codec_id == AV_CODEC_ID_HDMV_PGS_SUBTITLE) {
            decompressed = pgs_try_decompress(pkt.data, pkt.size, &decomp_size);
            if (decompressed) {
                decode_pkt.data = decompressed;
                decode_pkt.size = decomp_size;
            }
        }

        int ret = avcodec_decode_subtitle2(ps->sub_codec_ctx, &sub, &got_sub, &decode_pkt);

        if (ps->sub_codec_ctx->codec_id == AV_CODEC_ID_HDMV_PGS_SUBTITLE) {
            log_msg("Sub: MAIN-LOOP pkt_size=%d%s got_sub=%d rects=%u ret=%d seg=0x%02X",
                    pkt.size, decompressed ? " (zlib)" : "",
                    got_sub, got_sub ? sub.num_rects : 0, ret,
                    decode_pkt.size > 0 ? decode_pkt.data[0] : 0);
        } else {
            log_msg("Sub: MAIN-LOOP pkt_size=%d got_sub=%d rects=%u ret=%d",
                    pkt.size, got_sub, got_sub ? sub.num_rects : 0, ret);
        }

        av_free(decompressed);  /* NULL-safe */

        /* Track PGS packets fed this drain cycle */
        if (ps->sub_codec_ctx->codec_id == AV_CODEC_ID_HDMV_PGS_SUBTITLE) {
            pgs_packets_this_drain++;
            AVStream *pgs_st = ps->fmt_ctx->streams[ps->sub_active_idx];
            if (pkt.pts != AV_NOPTS_VALUE)
                last_pgs_pts = (double)pkt.pts * av_q2d(pgs_st->time_base);
        }
        if (ret < 0) {
            log_msg("Sub: decode error ret=%d", ret);
            av_packet_unref(&pkt);
            continue;
        }
        if (!got_sub) {
            /* Normal for PGS: decoder accumulates segments (PCS, WDS,
             * PDS, ODS) and only outputs on DISPLAY_SEGMENT (0x80). */
            av_packet_unref(&pkt);
            continue;
        }

        /* Compute display timing */
        AVStream *st = ps->fmt_ctx->streams[ps->sub_active_idx];
        double pkt_pts = 0.0;
        if (pkt.pts != AV_NOPTS_VALUE) {
            pkt_pts = (double)pkt.pts * av_q2d(st->time_base);
        }

        double start = pkt_pts + (double)sub.start_display_time / 1000.0;
        double end   = pkt_pts + (double)sub.end_display_time / 1000.0;

        /* SRT/subrip decoded by FFmpeg often sets end_display_time=0.
         * The actual duration is in pkt.duration in stream time_base. */
        if (sub.end_display_time == 0 && pkt.duration > 0) {
            end = pkt_pts + (double)pkt.duration * av_q2d(st->time_base);
        } else if (sub.end_display_time == 0) {
            end = start + 3.0;  /* last resort fallback */
        }

        /* PGS/DVB: end_display_time is often UINT32_MAX (duration unknown
         * until the clear packet arrives). Cap to 30s as a safety net —
         * the 0-rect clear packet will expire it earlier. */
        if (end - start > 30.0) {
            end = start + 30.0;
        }

        /* If we're only draining for a clear packet, handle it here
         * without touching the currently-displaying bitmap data. */
        if (draining_for_clear) {
            if (sub.num_rects == 0) {
                /* Found the clear signal */
                log_msg("Sub: clear signal (0 rects, pts=%.1f)", pkt_pts);
                if (pkt_pts > now) {
                    /* Clear is in the future — set the real end time.
                     * The sub will expire naturally via the time check. */
                    ps->sub_end_pts = pkt_pts;
                    avsubtitle_free(&sub);
                    av_packet_unref(&pkt);
                    break;
                }
                /* Clear is for now or past — expire immediately */
                ps->sub_valid = 0;
                sub_clear_bitmaps(ps);
                draining_for_clear = 0;
                avsubtitle_free(&sub);
                av_packet_unref(&pkt);
                continue;
            }
            /* Not a clear packet — skip it, keep looking */
            avsubtitle_free(&sub);
            av_packet_unref(&pkt);
            continue;
        }

        /* Extract text or bitmap data */
        char text[SUB_TEXT_SIZE] = {0};
        int got_bitmap = 0;

        /* Clear any previous bitmap textures */
        sub_clear_bitmaps(ps);

        for (unsigned i = 0; i < sub.num_rects; i++) {
            AVSubtitleRect *rect = sub.rects[i];

            if (rect->type == SUBTITLE_TEXT && rect->text) {
                snprintf(text, sizeof(text), "%s", rect->text);
                log_msg("Sub [TEXT] %.1f-%.1f: \"%.*s\"", start, end, 60, text);
            } else if (rect->type == SUBTITLE_ASS && rect->ass) {
                strip_ass_markup(rect->ass, text, sizeof(text));
                log_msg("Sub [ASS] %.1f-%.1f: \"%.*s\"", start, end, 60, text);
            } else if (rect->type == SUBTITLE_BITMAP &&
                       rect->data[0] && rect->data[1] &&
                       rect->w > 0 && rect->h > 0 &&
                       ps->sub_bitmap_count < MAX_SUB_BITMAPS) {
                /*
                 * Bitmap subtitles (PGS, VobSub, DVB):
                 *   rect->data[0] = pixel indices into palette
                 *   rect->data[1] = RGBA palette (4 bytes per entry, 0xAARRGGBB native)
                 *   rect->w/h     = dimensions
                 *   rect->x/y     = position relative to video frame
                 */
                uint32_t *palette = (uint32_t *)rect->data[1];
                int w = rect->w;
                int h = rect->h;

                /* Convert paletted pixels to RGBA */
                uint8_t *rgba = av_malloc(w * h * 4);
                if (rgba) {
                    for (int row = 0; row < h; row++) {
                        for (int col = 0; col < w; col++) {
                            uint8_t idx = rect->data[0][row * rect->linesize[0] + col];
                            uint32_t color = palette[idx];
                            int off = (row * w + col) * 4;
                            rgba[off + 0] = (color >> 16) & 0xFF;  /* R */
                            rgba[off + 1] = (color >> 8)  & 0xFF;  /* G */
                            rgba[off + 2] =  color        & 0xFF;  /* B */
                            rgba[off + 3] = (color >> 24) & 0xFF;  /* A */
                        }
                    }

                    /* Store RGBA data for GPU overlay compositing */
                    int bi = ps->sub_bitmap_count;
                    ps->sub_bitmap_data[bi] = rgba;  /* ownership transferred */
                    ps->sub_bitmap_w[bi] = w;
                    ps->sub_bitmap_h[bi] = h;
                    ps->sub_bitmap_rects[bi] = (SDL_Rect){ rect->x, rect->y, w, h };
                    ps->sub_bitmap_count++;
                    got_bitmap = 1;

                    log_msg("Sub [BITMAP] %.1f-%.1f: %dx%d at (%d,%d)",
                        start, end, w, h, rect->x, rect->y);
                }
            } else {
                log_msg("Sub: unknown rect type %d", rect->type);
            }
        }

        if (sub.num_rects == 0) {
            /* PGS/DVB: a 0-rect packet is the "clear" signal.
             * (Drain-for-clear case is handled above; this covers
             * clear packets encountered during normal scanning.) */
            log_msg("Sub: clear signal (0 rects, pts=%.1f)", pkt_pts);
            ps->sub_valid = 0;
            sub_clear_bitmaps(ps);
            avsubtitle_free(&sub);
            av_packet_unref(&pkt);
            continue;
        }

        avsubtitle_free(&sub);
        av_packet_unref(&pkt);

        if (text[0] == '\0' && !got_bitmap) continue;

        /* Skip subtitles that have already expired */
        if (end < now) {
            log_msg("Sub: skipped expired (end=%.1f < now=%.1f)", end, now);
            sub_clear_bitmaps(ps);
            continue;
        }

        /* Keep this subtitle */
        if (got_bitmap) {
            ps->sub_is_bitmap = 1;
            ps->sub_text[0] = '\0';
        } else {
            ps->sub_is_bitmap = 0;
            snprintf(ps->sub_text, sizeof(ps->sub_text), "%s", text);
        }
        ps->sub_start_pts = start;
        ps->sub_end_pts   = end;
        ps->sub_valid     = 1;
        break;  /* Show this one, leave rest in queue for later */
    }

    /* ── PGS post-drain: inject synthetic END segment ──
     * MKV muxers strip the zero-length END segment (0x80) that triggers
     * display set output in FFmpeg's PGS decoder. The decoder accumulates
     * PCS/WDS/PDS/ODS across calls but never fires without END.
     *
     * Key: only inject ONCE after draining real PGS packets — not every
     * idle frame. display_end_segment() resets presentation state, so a
     * premature END (before all segments arrive) would clear accumulated
     * data. By waiting until the queue is fully drained, all segments
     * from the current display set are loaded and END can assemble them. */
    if (pgs_packets_this_drain > 0 && !ps->sub_valid &&
        ps->sub_codec_ctx &&
        ps->sub_codec_ctx->codec_id == AV_CODEC_ID_HDMV_PGS_SUBTITLE) {

        static const uint8_t end_seg[] = { 0x80, 0x00, 0x00 };
        AVPacket end_pkt;
        memset(&end_pkt, 0, sizeof(end_pkt));
        end_pkt.data = (uint8_t *)end_seg;
        end_pkt.size = sizeof(end_seg);

        AVSubtitle sub;
        int got_sub = 0;
        int ret = avcodec_decode_subtitle2(ps->sub_codec_ctx, &sub, &got_sub, &end_pkt);
        log_msg("Sub: PGS-END inject after %d pkts: got_sub=%d rects=%u ret=%d last_pts=%.1f",
                pgs_packets_this_drain, got_sub, got_sub ? sub.num_rects : 0, ret, last_pgs_pts);

        if (ret >= 0 && got_sub) {
            double start = last_pgs_pts + (double)sub.start_display_time / 1000.0;
            double end   = last_pgs_pts + (double)sub.end_display_time / 1000.0;
            if (sub.end_display_time == 0) end = start + 5.0;
            if (end - start > 30.0) end = start + 30.0;

            if (sub.num_rects == 0) {
                log_msg("Sub: PGS-END clear (0 rects, pts=%.1f)", last_pgs_pts);
                ps->sub_valid = 0;
                sub_clear_bitmaps(ps);
                avsubtitle_free(&sub);
            } else {
                sub_clear_bitmaps(ps);
                int got_bitmap = 0;
                for (unsigned i = 0; i < sub.num_rects; i++) {
                    AVSubtitleRect *rect = sub.rects[i];
                    if (rect->type == SUBTITLE_BITMAP &&
                        rect->data[0] && rect->data[1] &&
                        rect->w > 0 && rect->h > 0 &&
                        ps->sub_bitmap_count < MAX_SUB_BITMAPS) {
                        uint32_t *palette = (uint32_t *)rect->data[1];
                        int w = rect->w, h = rect->h;
                        uint8_t *rgba = av_malloc(w * h * 4);
                        if (rgba) {
                            for (int row = 0; row < h; row++) {
                                for (int col = 0; col < w; col++) {
                                    uint8_t idx = rect->data[0][row * rect->linesize[0] + col];
                                    uint32_t color = palette[idx];
                                    int off = (row * w + col) * 4;
                                    rgba[off + 0] = (color >> 16) & 0xFF;
                                    rgba[off + 1] = (color >> 8)  & 0xFF;
                                    rgba[off + 2] =  color        & 0xFF;
                                    rgba[off + 3] = (color >> 24) & 0xFF;
                                }
                            }
                            int bi = ps->sub_bitmap_count;
                            ps->sub_bitmap_data[bi] = rgba;
                            ps->sub_bitmap_w[bi] = w;
                            ps->sub_bitmap_h[bi] = h;
                            ps->sub_bitmap_rects[bi] = (SDL_Rect){ rect->x, rect->y, w, h };
                            ps->sub_bitmap_count++;
                            got_bitmap = 1;
                            log_msg("Sub [PGS BITMAP] %.1f-%.1f: %dx%d at (%d,%d)",
                                    start, end, w, h, rect->x, rect->y);
                        }
                    }
                }
                avsubtitle_free(&sub);

                if (got_bitmap) {
                    ps->sub_is_bitmap = 1;
                    ps->sub_text[0] = '\0';
                    ps->sub_start_pts = start;
                    ps->sub_end_pts   = end;
                    ps->sub_valid     = 1;
                }
            }
        }
    }
}
