/*
 * DSVP — Dead Simple Video Player
 * dsvp.h — Shared types, constants, and declarations
 *
 * This header defines the central PlayerState and all supporting structures.
 * Every source file includes this.
 */

#ifndef DSVP_H
#define DSVP_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

/* FFmpeg libraries */
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libavutil/time.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/dovi_meta.h>

/* SDL3 — SDL_MAIN_HANDLED prevents SDL from injecting WinMain */
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_ttf/SDL_ttf.h>
#ifdef DSVP_HAVE_LIBASS
#include <ass/ass.h>            /* ASS/SSA typesetting renderer (optional, Makefile-detected) */
#endif

/* SDL3 shadercross — runtime HLSL→SPIRV→native compilation */
#include <SDL3_shadercross/SDL_shadercross.h>

/* ── Constants ──────────────────────────────────────────────────────── */

#define DSVP_VERSION        "0.3.9-beta"
#define DSVP_WINDOW_TITLE   "DSVP"

#define PACKET_QUEUE_MAX    256     /* max packets buffered per stream  */
#define FRAME_QUEUE_MAX     12      /* decoded video frame buffer depth */
#define AUDIO_BUF_SIZE      192000  /* max decoded audio buffer bytes   */
#define SEEK_STEP_SEC       5.0     /* arrow key seek increment         */
#define VOLUME_STEP         0.05    /* arrow key volume increment       */

#define MAX_SUB_STREAMS     16      /* max subtitle tracks to catalog   */
#define MAX_AUDIO_STREAMS   16      /* max audio tracks to catalog      */
#define SUB_TEXT_SIZE       4096    /* max subtitle text buffer         */
#define MAX_SUB_BITMAPS     4       /* max bitmap rects per subtitle    */
#define SUB_MAX_ACTIVE_CUES 8       /* concurrent text cues (review M4)  */
#define SUB_CUE_TEXT_SIZE   1024    /* one text cue                      */
typedef struct { char text[SUB_CUE_TEXT_SIZE]; double start, end; } SubCue;

/* T5: an explicit A / S choice, carried to the next file in the folder.
 * Matched by language + title, then language, then (untagged) title or
 * position — never by stream index, which means nothing across files. */
typedef struct { int set; int pos; char lang[16]; char title[96]; } TrackPref;
#define TRACKS_APPLY_NONE   0   /* O, argv: the file's own defaults           */
#define TRACKS_APPLY_PREF   1   /* N/B, auto-play: carry the explicit choices */
#define TRACKS_APPLY_RESUME 2   /* R: the record's exact streams, then prefs  */

/* Default window size when no video is loaded */
#define DEFAULT_WIN_W       960
#define DEFAULT_WIN_H       540

/* ── Bitstream Audio Types ──────────────────────────────────────────
 *
 * AudioMode controls how audio reaches the output device:
 *   PCM         — always decode to F32 stereo (current default behavior)
 *   AUTO        — probe HDMI sink via EDID; passthrough if supported, else PCM
 *   PASSTHROUGH — force passthrough; falls back to PCM on handshake failure
 *
 * BitstreamCaps is populated by bitstream_probe() (phase 2) from the
 * EDID Short Audio Descriptors reported by the connected HDMI sink.
 */

typedef enum {
    AUDIO_MODE_PCM         = 0,   /* decode → swr → F32 stereo (safe default) */
    AUDIO_MODE_AUTO        = 1,   /* probe sink, passthrough if possible       */
    AUDIO_MODE_PASSTHROUGH = 2    /* force passthrough, fallback on failure    */
} AudioMode;

typedef struct BitstreamCaps {
    int  support_ac3;      /* sink decodes AC-3 (Dolby Digital)         */
    int  support_eac3;     /* sink decodes E-AC-3 (DD+)                */
    int  support_truehd;   /* sink decodes TrueHD (lossless / Atmos)   */
    int  support_dts;      /* sink decodes DTS core                    */
    int  support_dtshd;    /* sink decodes DTS-HD MA (lossless)        */
    int  hbr_capable;      /* HDMI supports High Bit Rate (TrueHD req) */
    int  max_channels;     /* max channel count reported by sink       */
    int  probed;           /* 1 = caps have been queried this session   */
    int  platform_unsupported; /* 1 = no probe exists here (Windows); the
                                  panel says so and audio_open stops asking */
} BitstreamCaps;

/* ── Packet Queue ───────────────────────────────────────────────────
 *
 * Thread-safe FIFO queue for AVPackets. The demux thread pushes packets,
 * and the video/audio decode paths pop them. Uses SDL mutex + condvar
 * because SDL's threading is cross-platform (no need for pthreads).
 */

typedef struct PacketNode {
    AVPacket           *pkt;
    struct PacketNode  *next;
} PacketNode;

typedef struct PacketQueue {
    PacketNode  *first;
    PacketNode  *last;
    int          nb_packets;
    int          size;          /* total byte size of queued packet data */
    SDL_Mutex   *mutex;
    SDL_Condition *cond;
    int          abort_request; /* signal threads to stop blocking      */
} PacketQueue;

/* ── Frame Queue ────────────────────────────────────────────────────
 *
 * Thread-safe FIFO queue for decoded AVFrames. The video decode
 * thread pushes finished frames; the main display thread pops them.
 * Bounded to FRAME_QUEUE_MAX entries — fq_put blocks when full,
 * naturally throttling the decoder to display rate while providing
 * burst absorption when decode runs ahead of realtime.
 */

typedef struct FrameNode {
    AVFrame           *frame;
    struct FrameNode  *next;
} FrameNode;

typedef struct FrameQueue {
    FrameNode     *first;
    FrameNode     *last;
    int            nb_frames;
    int            flush_serial;  /* incremented on fq_flush; fq_put returns -2 if changed mid-wait */
    int            last_pop_serial; /* serial observed by fq_get at pop time (consumer-side seek race) */
    SDL_Mutex     *mutex;
    SDL_Condition *cond;
    int            abort_request; /* signal threads to stop blocking */
} FrameQueue;

/* ── GPU Uniform Data ──────────────────────────────────────────────
 *
 * Pushed to the fragment shader each frame via SDL_PushGPUFragmentUniformData.
 * Layout must match the HLSL cbuffer exactly (std140-ish packing).
 */

/* Build stamp: the Makefile always passes -DDSVP_GIT_COMMIT; this
 * fallback only covers a bare compiler invocation. Shown in the log
 * banner (main.c) and the debug overlay (player.c). */
#ifndef DSVP_GIT_COMMIT
#define DSVP_GIT_COMMIT "unknown"
#endif

typedef struct GPUUniforms {
    float colorMatrix[16];  /* 4×4 YUV→RGB matrix (row-major)   64 bytes */
    float rangeY[2];        /* { offset, scale } for Y plane      8 bytes */
    float rangeUV[2];       /* { offset, scale } for UV planes    8 bytes */
    float texSizeY[2];      /* { width, height } of Y texture     8 bytes */
    float texSizeUV[2];     /* { width, height } of UV textures   8 bytes */
    float chromaOffset[2];  /* chroma siting correction (texels)  8 bytes */
    float frameCount;       /* frame counter for temporal dither  4 bytes */
    float is_hdr;           /* 1.0 = HDR content detected         4 bytes */
    float hdr_peak_nits;    /* source peak luminance (nits)       4 bytes */
    float hdr_gamut;        /* 0.0=BT.709, 1.0=BT.2020 primaries 4 bytes */
    float hdr_debug;        /* 0-3: HDR debug viz mode             4 bytes */
    float hdr_target_nits;  /* SDR display peak (T key toggle)     4 bytes */
    float hdr_midtone_gain; /* midtone lift exponent (G key)       4 bytes */
    float is_dovi;          /* 1.0 = DV reshaping active           4 bytes */
    float out_gamma;        /* 0 = sRGB piecewise; else power exp  4 bytes */
    float is_hlg;           /* 1.0 = HLG transfer (ARIB STD-B67)   4 bytes */
    /* (these two occupy what was explicit padding — the 144B float4
     *  alignment boundary below is unchanged) */
    /* ── 144B boundary ── */
    float dovi_num_pieces[4]; /* [I, Ct, Cp, 0] piece counts      16 bytes */
    float dovi_pivots[9][4];  /* [pivot][comp] normalized pivots 144 bytes */
    float dovi_c0[8][4];     /* [piece][comp] poly coef c0       128 bytes */
    float dovi_c1[8][4];     /* [piece][comp] poly coef c1       128 bytes */
    float dovi_c2[8][4];     /* [piece][comp] poly coef c2       128 bytes */
    float dovi_ycc_r0[4];   /* ycc→rgb row 0 [m,m,m,offset]      16 bytes */
    float dovi_ycc_r1[4];   /* ycc→rgb row 1 [m,m,m,offset]      16 bytes */
    float dovi_ycc_r2[4];   /* ycc→rgb row 2 [m,m,m,offset]      16 bytes */
    float dovi_out_r0[4];   /* output row 0 [m,m,m,0] (lms→2020) 16 bytes */
    float dovi_out_r1[4];   /* output row 1 [m,m,m,0]            16 bytes */
    float dovi_out_r2[4];   /* output row 2 [m,m,m,0]            16 bytes */
    /* ── DV MMR chroma reshaping (appended — offsets above unchanged) ──
     * Real-world P5 RPUs overwhelmingly reshape Ct/Cp with MMR, a
     * cross-channel polynomial over the full (I,Ct,Cp) triple; the
     * per-component poly path cannot represent it. Single-piece MMR
     * (the universal case for chroma) is supported; order 1-3. */
    float dovi_mmr_meta[4];   /* [ct_order, cp_order, ct_const, cp_const] */
    float dovi_mmr_ct[6][4];  /* 21 coeffs, [order*7+term] packed  96 bytes */
    float dovi_mmr_cp[6][4];  /*                                   96 bytes */
    /* ── appended (offsets above unchanged) ── */
    float hdr_pass;           /* 1.0 = HDR10/PQ passthrough swapchain    */
    /* BT.2390 black-level lift (deck 4614882 port). <0 = off
     * (DSVP_BLACK_NITS=0); 0 = auto target/2000 (unreached on x64 —
     * the CPU default is FIXED 0.01 nits, Holden's WIN11 IPS ladder
     * pick 2026-08-27; deck keeps auto); >0 = fixed nits.
     * Takes a pad slot — offsets and total size unchanged. Run
     * tools/check-uniforms.py after ANY edit here. */
    float out_black_nits;
    float _pad_hdrp[2];       /* float4 register pad                     */
} GPUUniforms;              /*                                 1008 bytes */

/* ── Player State ───────────────────────────────────────────────────
 *
 * Central structure holding everything: format/codec contexts, queues,
 * SDL handles, clocks, and UI state. One instance per playback session.
 */

typedef struct PlayerState {
    /* ── Format / streams ── */
    AVFormatContext    *fmt_ctx;
    int                 video_stream_idx;
    int                 audio_stream_idx;

    /* ── Video decode ── */
    AVCodecContext     *video_codec_ctx;
    struct SwsContext  *sws_ctx;
    int                 sws_out_10bit;   /* sws dst is yuv420p10le (deep sources
                                            keep 10-bit precision through the
                                            R16 path instead of being crushed
                                            to 8-bit before PQ decode) */
    int                 sws_dst_siting;  /* AVChromaLocation the sws OUTPUT is
                                            pinned to via dst_chr_pos — drives
                                            the shader siting offset */

    /* ── Deinterlacing (bwdif via lavfi) — content-aware ──
     * Graph is created lazily by the decode thread the first time a
     * frame arrives flagged interlaced, and freed on seek (bwdif keeps
     * temporal field state). deint=interlaced passes progressive frames
     * through untouched, so mixed streams are handled per frame.
     * DSVP_DEINT=0 disables, =1 forces the graph on from frame one. */
    struct AVFilterGraph   *deint_graph;
    struct AVFilterContext *deint_src;
    struct AVFilterContext *deint_sink;
    int                     deint_disabled;
    int                     deint_force;
    AVFrame            *video_frame;      /* raw decoded frame          */
    AVFrame            *rgb_frame;        /* scaled/converted for SDL   */
    uint8_t            *rgb_buffer;       /* backing buffer for rgb_frame */

    /* ── Audio decode ── */
    AVCodecContext     *audio_codec_ctx;
    struct SwrContext  *swr_ctx;
    /* Input format swr_ctx was configured for — mid-stream changes
     * (broadcast TS 5.1<->2.0 at program boundaries, AAC SBR rate
     * switches) must rebuild the resampler, not feed it stale-config
     * frames (planar-channel mismatch reads NULL plane pointers). */
    AVChannelLayout     swr_in_layout;
    int                 swr_in_format;
    int                 swr_in_rate;
    AVFrame            *audio_frame;
    uint8_t            *audio_buf;        /* resampled audio buffer     */
    unsigned int        audio_buf_cap;    /* allocated capacity of audio_buf (bytes) */
    unsigned int        audio_buf_size;   /* bytes of valid data in buf */
    unsigned int        audio_buf_index;  /* read cursor into buf       */

    /* ── Bitstream passthrough ── */
    AudioMode           audio_mode;       /* PCM / Auto / Passthrough   */
    BitstreamCaps       bitstream_caps;   /* HDMI sink capabilities     */
    int                 bitstream_active; /* 1 = currently passing through */

    /* ── Packet queues ── */
    PacketQueue         video_pq;
    PacketQueue         audio_pq;

    /* ── Decoded video frame queue (decode thread → display thread) ── */
    FrameQueue          video_frame_q;

    /* ── Audio stream catalog ── */
    int                 aud_stream_indices[MAX_AUDIO_STREAMS];
    char                aud_stream_names[MAX_AUDIO_STREAMS][128];
    int                 aud_count;          /* number of audio streams      */
    int                 aud_selection;      /* 0-based index into catalog   */

    /* Audio track change OSD */
    char                aud_osd[256];
    double              aud_osd_until;

    /* ── SDL handles ── */
    SDL_Window         *window;
    SDL_AudioStream    *audio_stream;    /* SDL3: owns the device        */
    SDL_AudioSpec       audio_spec;       /* actual device spec           */
    /* AUDCLK instrument (review DM19/DM20 inputs, log-only): the
     * callback's last clock-correction inputs, read by main at the
     * DIAG cadence. Written on the audio thread; aligned scalars,
     * same tolerance as audio_clock_sync.                          */
    double              aud_diag_buffered; /* pre-clamp buffered seconds  */
    int                 aud_diag_internal; /* bytes decoded, not pushed   */
    int                 aud_diag_stream;   /* bytes queued in SDL stream  */
    int                 aud_diag_clamped;  /* snapshots that hit the cap  */

    /* ── SDL_GPU handles (lifetime: application) ── */
    SDL_GPUDevice              *gpu_device;
    SDL_GPUGraphicsPipeline    *gpu_pipeline_yuv;   /* planar YUV420P   */
    SDL_GPUGraphicsPipeline    *gpu_pipeline_blit;  /* frame cache → swapchain copy */
    /* (linear gpu_sampler deleted — deck d2f5351 port: kernels filter,
     * everything binds gpu_sampler_nearest; x64 has no PQ LUTs) */
    SDL_GPUSampler             *gpu_sampler_nearest; /* nearest for overlay */

    /* ── SDL_GPU handles (lifetime: per-file, created/destroyed in player_open/close) ── */
    SDL_GPUTexture             *gpu_tex_y;           /* Y plane          */
    SDL_GPUTexture             *gpu_tex_u;           /* U plane          */
    SDL_GPUTexture             *gpu_tex_v;           /* V plane          */
    SDL_GPUTexture         *gpu_tex_noise;           /* 64×64 blue noise dither (app lifetime) */
    SDL_GPUTransferBuffer      *gpu_xfer_y;          /* CPU→GPU staging  */
    SDL_GPUTransferBuffer      *gpu_xfer_u;
    SDL_GPUTransferBuffer      *gpu_xfer_v;
    Uint32                      gpu_xfer_y_cap;      /* staging capacities (bytes) — */
    Uint32                      gpu_xfer_uv_cap;     /* allow stride-padded single memcpy */
    GPUUniforms                 gpu_uniforms;         /* current color params */

    /* ── Shaded-frame cache ──
     * The YUV→RGB (+tone map) shader output is rendered once per CONTENT
     * frame into this texture; reblit ticks blit the cached result
     * instead of re-running the 48-tap shader every VSync. Output is
     * identical: frameCount only advances on new frames, so reblits
     * always reproduced the same shading anyway. Invalidated on resize,
     * letterbox change, or HDR uniform change (H/T/G keys). */
    SDL_GPUTexture             *gpu_tex_cache;       /* sized to swapchain      */
    int                         cache_w, cache_h;    /* current cache dims      */
    int                         cache_valid;         /* 1 = holds current frame */
    SDL_Rect                    cache_rect;          /* display_rect at render  */

    /* ── HDR dynamic peak detection (Layer 1: CPU scan) ── */
    float                       hdr_smoothed_peak;    /* temporally smoothed peak (nits) */
    float                       hdr_prev_frame_peak;  /* raw peak from previous frame    */
    float                       hdr_static_peak;      /* metadata peak (fallback ceiling) */
    int                         hdr_meta_pending;     /* 1 = open found no static metadata;
                                                         adopt MDCV/CLL from the first presented
                                                         frame's side data (SEI-only streams, H) */
    int                         hdr_target_idx;       /* index into SDR target nit table  */
    int                         dovi_metadata_logged; /* 1 = logged DV RPU for this file  */
    float                       dovi_l1_peak_nits;    /* DV L1 authored scene peak (nits);
                                                         0 = no L1 seen this file. Drives
                                                         the tone map ahead of histogram/
                                                         static (deck a4a90b5 port) */
    double                      dovi_l1_last_log;     /* L1 scene-change log throttle */
    double                      black_relearn_at;   /* batch J learn-at-Z: next deferred probe (wall clock), 0 = none */
    int                         black_relearn_left; /* probes still to try after the HDR switch */
    const char                 *black_src;            /* black-lift source label for
                                                         the engage log (default/env) */
    int                         hdr_pass_content;     /* current file's signal is HDR
                                                         (PQ/HLG/DV) — drives system
                                                         display-HDR engage (deck lineage) */
    int                         hdr_out_mode;         /* 0 = off (tone-map), 1 = auto.
                                                         Z toggles at runtime */
    int                         hdr_out_active;       /* swapchain is currently
                                                         HDR10/ST2084 */

    /* ── Overlay GPU handles (lifetime: application, resized as needed) ── */
    SDL_GPUGraphicsPipeline    *gpu_pipeline_overlay; /* RGBA + alpha blend */
    SDL_GPUTexture             *gpu_overlay_tex;      /* RGBA8888 overlay  */
    SDL_GPUTransferBuffer      *gpu_overlay_xfer;     /* CPU→GPU staging   */
    int                         overlay_tex_w;         /* current texture dimensions */
    int                         overlay_tex_h;
    int                         overlay_dirty;         /* 1 = need re-upload */
    int                         overlay_up_y0;         /* pending upload rows */
    int                         overlay_up_y1;         /* (exclusive)         */

    /* ── Timing / A/V sync ── */
    double              audio_clock;      /* current audio PTS in secs (audio thread internal) */
    double              audio_clock_sync; /* latency-corrected snapshot for main thread A/V sync */
    double              av_bias;          /* adaptive A/V offset (EMA of av_diff) */
    int                 av_bias_samples;  /* warmup counter (apply after 60)     */
    int                 av_settled_samples; /* samples since the last seek landed or
                                               timed out: the run-peak gate ((B)
                                               2026-10-06 — av_bias_samples never
                                               reset at a seek, so it gated only the
                                               first half-second of the file) */
    int                 av_hold;          /* 1 = estimator frozen across a display
                                           toggle stall (Z) until the lag clears */
    int                 av_hold_frames;   /* frames evaluated under the hold; 120 =
                                           fail-open release                     */
    int                 av_trace_left;    /* AVTRACE lines still to print after a
                                           hold release (per-frame instrument)   */
    double              audio_pts_floor;  /* post-seek: discard audio frames with PTS below this */
    double              video_clock;      /* current video PTS in secs  */
    double              frame_timer;      /* when we last showed a frame*/
    double              frame_last_delay; /* last frame display duration*/
    double              frame_last_pts;   /* PTS of last displayed frame*/
    int64_t             seek_target;      /* seek target in AV_TIME_BASE*/
    int                 seek_request;     /* 1 = seek pending           */
    int                 seek_flags;
    int                 seek_recovering;  /* 1 = waiting for first displayed frame post-seek */
    double              seek_recovering_start; /* wall-clock when seek_recovering=1 was set (timeout fallback) */
    double              seek_recover_last_frame; /* wall-clock of the last frame decoded while
                                                  recovering: the 2 s fail-open counts from
                                                  here, not from the seek (T2, 2026-10-06) */
    /* SEEKDIAG one-shots (deck 20325d1 port): the recovery contract's
     * live inputs, logged per seek. The deck's seek-recovery week
     * convicted three races with these; vocabulary kept identical
     * across repos so the same greps work. */
    int                 seekdiag_vid_pending; /* 1 = log first post-seek video frame PTS */
    int                 seekdiag_aud_pending; /* 1 = log first post-seek audio frame PTS */
    int64_t             seekdiag_target;  /* the target the demuxer actually SERVICED,
                                           stored when the frame queue is flushed
                                           (the serial change) — (A) 2026-10-06:
                                           stored at pickup it labelled the previous
                                           seek's frames with the new target      */
    int                 seekdiag_backward;/* that seek's BACKWARD flag, same moment
                                             (seek_flags is live, like seek_target) */
    int                 seek_inflight;    /* 1 = demux picked a request up and has not
                                             flushed yet: frames popped now are the
                                             PREVIOUS seek's, under the old serial */
    int                 seekdiag_target_valid; /* 1 = seek_target is from a real seek this file
                                                  (gates target/landed in the recovery log) */
    /* Frame step (T2, `,` / `.`; main.c). All main-thread only. */
    int                 step_fwd_pending; /* 1 = `.` waits for the next decoded frame */
    double              step_fwd_since;   /* wall clock the pending step began (fail-open) */
    int                 step_seek_pending;/* 1 = an exact-step seek is landing: frames
                                             below step_target are decoded, not shown */
    double              step_target;      /* PTS (s) the exact step must reach */
    int                 step_dirty;       /* 1 = frames advanced by pop while paused:
                                             audio needs a resync before it resumes */

    /* ── Threads ── */
    SDL_Thread         *demux_thread;
    SDL_Thread         *video_decode_thread;
    SDL_Mutex          *seek_mutex;    /* protects codec flush vs decode  */
    SDL_Mutex          *seek_req_mutex;/* m-S1-a: the request slot only (seek_target/
                                          flags/request + the pickup's inflight
                                          mark), held for a few instructions —
                                          never seek_mutex, which the decoder
                                          holds across a whole 4K frame */
    int                 seeking;       /* 1 = flush in progress, skip decode */

    /* ── Playback state ── */
    int                 playing;          /* 1 = file is loaded/playing */
    int                 paused;
    int                 quit;             /* 1 = application exiting    */
    double              volume;           /* 0.0 — 1.0                  */
    int                 fullscreen;
    int                 eof;              /* demuxer hit end of file    */
    int                 io_error;         /* persistent decode failure — main.c tears down */
    int                 res_change_logged; /* per-file: mid-stream size warn shown  */
    int                 expected_pix_fmt;  /* AVPixelFormat textures were sized for —
                                            * mid-stream format changes are skipped
                                            * like resolution changes (upload paths
                                            * would overrun the transfer buffer)    */
    int                 video_frame_serial; /* flush_serial of the displayed frame —
                                             * seek recovery accepts only a frame
                                             * popped after the flush              */
    float               ui_display_scale;   /* (D) SDL_GetWindowDisplayScale — the OS
                                               scale setting x pixel density; main
                                               refreshes it on DISPLAY_SCALE_CHANGED */
    int                 overlay_force_full; /* overlay texture just (re)created:
                                             * GPU contents undefined, next render
                                             * must clear + upload full height     */
    int                 cache_fail_logged; /* per-file: frame-cache create fail shown */
    int                 video_ready;      /* 1 after first frame uploaded — gates reblit */

    /* ── Window geometry ── */
    int                 win_w, win_h;     /* current window size        */
    int                 vid_w, vid_h;     /* video native resolution    */
    int                 chroma_location;  /* AVChromaLocation for debug overlay */
    SDL_Rect            display_rect;     /* letterboxed video area     */
    int                 sc_w, sc_h;       /* last swapchain dims (physical pixels) */

    /* ── Overlay visibility state ── */
    int                 show_debug;
    int                 show_info;
    int                 show_keys;          /* T6: K — the key-bindings panel */
    int                 show_seekbar;         /* 1 = seek bar visible       */
    double              seekbar_hide_time;    /* auto-hide after this time  */
    int                 seekbar_track_x;      /* progress track left edge   */
    int                 seekbar_track_w;      /* progress track width       */
    int                 overlay_active;       /* 1 = overlay has content    */

    /* ── Subtitles ── */
    int                 sub_stream_indices[MAX_SUB_STREAMS];
    char                sub_stream_names[MAX_SUB_STREAMS][128];
    int                 sub_count;          /* number of subtitle streams   */
    int                 sub_selection;      /* user selection: 0=off, 1..N  */
    int                 sub_active_idx;     /* AVStream index or -1         */
    AVCodecContext     *sub_codec_ctx;
    PacketQueue         sub_pqs[MAX_SUB_STREAMS]; /* one queue per stream */

    /* Active TEXT cues (review M4): several can be on screen at once —
     * two speakers, a sign over dialogue — each with its own window.
     * sub_text below is the JOINED render string rebuilt whenever the
     * set changes; sub_start_pts/sub_end_pts span the set. */
    SubCue              sub_cues[SUB_MAX_ACTIVE_CUES];
    int                 sub_cue_count;

    /* Current subtitle display */
    char                sub_text[SUB_TEXT_SIZE];
    double              sub_start_pts;      /* show from this PTS           */
    double              sub_end_pts;        /* hide after this PTS          */
    int                 sub_valid;          /* 1 = sub_text should display  */
    int                 sub_is_bitmap;      /* 1 = bitmap sub, 0 = text     */

    /* Bitmap subtitle pixel data (RGBA, freed via av_free) */
    uint8_t            *sub_bitmap_data[MAX_SUB_BITMAPS];
    int                 sub_bitmap_w[MAX_SUB_BITMAPS];
    int                 sub_bitmap_h[MAX_SUB_BITMAPS];
    SDL_Rect            sub_bitmap_rects[MAX_SUB_BITMAPS];
    int                 sub_bitmap_count;

    /* ASS/SSA typesetting via libass (2026-09 cycle — the end user's
     * JoJo sign/SFX ask). Library + renderer live per FILE (embedded
     * fonts are per file), the track per OPENED ASS stream. Every
     * libass call is MAIN THREAD only (decode drain, overlay draw,
     * S-cycle, close). Built without libass, or DSVP_NO_LIBASS=1 →
     * the pre-libass path (tags stripped, house style). */
#ifdef DSVP_HAVE_LIBASS
    ASS_Library        *ass_lib;
    ASS_Renderer       *ass_rend;
    ASS_Track          *ass_track;
#endif
    int                 sub_ass_active;     /* 1 = active track renders via libass */
    int                 ass_frame_w, ass_frame_h; /* last frame size given to libass */
    int                 ass_stor_w, ass_stor_h;   /* last storage size given to libass */
    int                 ass_fonts_embedded; /* attachments handed to libass this file */
    int                 ass_msg_count;      /* libass warnings logged this file (capped) */
    int                 ass_nopts_logged;   /* once-per-file: event without PTS/duration */

    /* Track change OSD */
    char                sub_osd[256];       /* "Subtitles: English" etc.    */
    double              sub_osd_until;      /* hide OSD after this time     */

    /* ── Media info cache ── */
    char                filepath[1024];
    /* Resume-last-file (end-user request 2026-09-07): the ONE file
     * DSVP remembers, and where in it. Loaded at startup from
     * dsvp.resume (next to the executable when writable, else the
     * per-user state dir); shown on the idle screen; R opens it.
     * DSVP_NO_RESUME=1 → never read, never written. */
    char                resume_path[1024];
    double              resume_pos;
    int                 resume_aud_stream;  /* T5: active streams when the record was */
    int                 resume_sub_stream;  /* written; -2 = not recorded, sub -1 = off */
    TrackPref           pref_aud;           /* T5: survive close/open (main + player.c) */
    TrackPref           pref_sub;
    int                 track_apply;        /* TRACKS_APPLY_*, set around player_open  */

    /* Chapters (end-user request 2026-09-07: "segment support" = MKV
     * chapters). Read from the container at open; PgUp/PgDn step them,
     * the OSD names them, the media info lists them. */
#define DSVP_MAX_CHAPTERS 256
    int                 nb_chapters;
    double              chapter_start[DSVP_MAX_CHAPTERS];   /* seconds */
    char                chapter_title[DSVP_MAX_CHAPTERS][64];
    int                 chapter_nav_idx;  /* chapter last jumped to, -1 none:
                                             a backward seek lands BEFORE the
                                             chapter start, so for the pre-roll
                                             we are already "in" that chapter
                                             (field 2026-09-07: PgDn was stuck) */
    char                media_info[8192]; /* formatted info string      */
    char                debug_info[4096]; /* formatted debug string     */

    /* ── Playback diagnostics ── */
    int                 diag_frames_displayed; /* total frames shown       */
    int                 diag_frames_decoded;   /* total frames decoded     */
    int                 diag_frames_dropped;   /* frames decoded but not shown */
    int                 diag_multi_decodes;    /* ticks with >1 decode     */
    int                 diag_timer_snaps;      /* frame_timer snap-forwards*/
    double              diag_max_av_drift;     /* worst A/V drift (signed) */
    double              diag_last_report;      /* time of last periodic log*/

    /* ── Profiling (make profile → -DDSVP_PROFILE; deck port) ──
     * Per-frame section timings for video_display / video_reblit /
     * the decode thread, accumulated between 10s DIAG reports.
     * decode_* is written from the decode thread without locking —
     * a torn read costs one bad sample in a diagnostic, not state. */
    double              prof_upload_ms;       /* last frame: convert+upload */
    double              prof_peak_ms;         /* last frame: scene-peak scan */
    double              prof_vsync_ms;        /* last frame: acquire (VSync) */
    double              prof_render_ms;       /* last frame: render + submit */
    double              prof_display_ms;      /* last frame: video_display() */
    int                 prof_n;               /* display ticks in window     */
    double              prof_sum_upload,  prof_max_upload;
    double              prof_sum_peak,    prof_max_peak;
    double              prof_sum_vsync,   prof_max_vsync;
    double              prof_sum_render,  prof_max_render;
    double              prof_sum_total,   prof_max_total;
    int                 prof_dec_n;           /* decoded frames in window    */
    double              prof_sum_decode,  prof_max_decode;
    int                 prof_rb_n;            /* reblit ticks in window      */
    double              prof_sum_rb_vsync, prof_max_rb_vsync;
    double              prof_sum_rb_total, prof_max_rb_total;

    /* ── Real-time FPS measurement (debug overlay, 0.5s window) ── */
    double              fps_window_start;      /* window anchor (wall sec) */
    int                 fps_window_frames;     /* CONTENT frames in window */
    int                 rfps_window_frames;    /* GPU presents in window   */
    double              fps_content;           /* measured content fps     */
    double              fps_render;            /* measured present rate    */

    /* ── Folder playlist (prev/next navigation) ── */
    char              **playlist_files;      /* sorted full paths          */
    int                 playlist_count;      /* number of playable files   */
    int                 playlist_index;      /* current file's index (-1)  */

} PlayerState;

/* ── Packet Queue API ─────────────────────────────────────────────── */

void  pq_init(PacketQueue *q);
void  pq_destroy(PacketQueue *q);
int   pq_put(PacketQueue *q, AVPacket *pkt);
int   pq_peek_pts(PacketQueue *q, int64_t *pts_out);
void  pq_prune_stale(PacketQueue *q, int64_t min_pts);
void  deint_graph_free(PlayerState *ps);
int   pq_get(PacketQueue *q, AVPacket *pkt, int block);
void  pq_flush(PacketQueue *q);

/* ── Frame Queue API ──────────────────────────────────────────────── */

void  fq_init(FrameQueue *q);
void  fq_destroy(FrameQueue *q);
int   fq_put(FrameQueue *q, AVFrame *frame, int block, int expect_serial);
int   fq_get(FrameQueue *q, AVFrame **frame_out, int block);
void  fq_flush(FrameQueue *q);

/* ── Player API (player.c) ────────────────────────────────────────── */

int   player_open(PlayerState *ps, const char *filename);
void  player_close(PlayerState *ps);
int   demux_thread_func(void *arg);
int   video_decode_thread_func(void *arg);
int   video_decode_frame(PlayerState *ps);
void  video_display(PlayerState *ps);
void  video_reblit(PlayerState *ps);
void  player_seek(PlayerState *ps, double incr);
void  player_seek_abs(PlayerState *ps, double pos_sec, int backward); /* absolute; backward = land on the keyframe at/before pos */
int   player_chapter_index(const PlayerState *ps, double pos_sec);   /* -1 when no chapters */
void  player_build_media_info(PlayerState *ps);
void  player_build_debug_info(PlayerState *ps);
void  player_update_display_rect(PlayerState *ps);

/* ── GPU Init (player.c) ──────────────────────────────────────────── */

int   gpu_create_pipelines(PlayerState *ps);
void  gpu_destroy_pipelines(PlayerState *ps);
void  hdr_sys_reconcile_stamp(void);
void  hdr_black_lift_on_hdr_state(PlayerState *ps, int on);
void  hdr_black_lift_tick(PlayerState *ps, double now);   /* batch J: deferred re-probes after an HDR switch (1, 2, 4 s) */ /* batch J learn-at-Z: on=1 probes the display's black with the desktop in HDR and keeps it; every flip re-decides the current file's lift */
int   hdr_sys_os_hdr_active(PlayerState *ps); /* (I): OS HDR active on the window's monitor — 1/0, -1 unknown (Windows: DisplayConfig, read-only; Linux: -1) */
int   hdr_sys_held_sdrbr(void);             /* KWin reference-luminance we hold (0 = none; Windows always 0) */         /* crash-restore: reconcile a dead
                                                session's display stamp at launch,
                                                BEFORE any probe (no-op on Windows) */
void  hdr_output_apply(PlayerState *ps);     /* engage/revert system display HDR
                                                per content (Windows; deck lineage) */
void  hdr_output_shutdown(PlayerState *ps);  /* app-exit revert */

/* ── Overlay GPU (player.c) ──────────────────────────────────────── */

int   gpu_overlay_ensure(PlayerState *ps, int width, int height);
void  gpu_overlay_upload(PlayerState *ps, const uint8_t *rgba, int width, int height, int y0, int y1);
void  gpu_overlay_copy_cmd(SDL_GPUCommandBuffer *cmd, PlayerState *ps);
void  gpu_overlay_draw(SDL_GPURenderPass *pass, SDL_GPUCommandBuffer *cmd,
                        PlayerState *ps, Uint32 sc_w, Uint32 sc_h);
void  gpu_overlay_destroy(PlayerState *ps);

/* ── Audio API (audio.c) ──────────────────────────────────────────── */

int   audio_open(PlayerState *ps);
void  audio_close(PlayerState *ps);
void  SDLCALL audio_callback(void *userdata, SDL_AudioStream *stream,
                              int additional_amount, int total_amount);
int   audio_decode_frame(PlayerState *ps);
void  audio_find_streams(PlayerState *ps);
void  audio_cycle(PlayerState *ps);
void  track_pref_note(PlayerState *ps, int is_sub, int stream_idx, int pos);
void  track_apply_subs(PlayerState *ps);

/* ── Bitstream API (bitstream.c) ─────────────────────────────────── */

int   bitstream_probe(BitstreamCaps *caps);
int   bitstream_can_passthrough(const BitstreamCaps *caps,
                                 enum AVCodecID codec_id);

/* ── Subtitle API (subtitle.c) ───────────────────────────────────── */

void  sub_find_streams(PlayerState *ps);
int   sub_open_codec(PlayerState *ps, int stream_idx);
void  sub_close_codec(PlayerState *ps);
void  sub_cycle(PlayerState *ps);
int   sub_select(PlayerState *ps, int sel);   /* 0 = off, 1..sub_count */
void  sub_decode_pending(PlayerState *ps);
void  sub_clear_display(PlayerState *ps);   /* drop cues + bitmaps + text (seek, close) */
void  sub_ass_close_file(PlayerState *ps);  /* per-file libass library + renderer (player_close) */
#ifdef DSVP_HAVE_LIBASS
ASS_Image *sub_ass_render(PlayerState *ps, int frame_w, int frame_h,
                          double now_sec, int *changed);
#endif
int   sub_init_font(void);
void  sub_close_font(void);
TTF_Font *sub_get_font(void);
TTF_Font *sub_get_outline_font(void);
void  sub_set_font_size(int font_size);
int   ui_scale_for(const PlayerState *ps, int sc_h);

/* ── Overlay API (overlay.c) ─────────────────────────────────────── */

void  overlay_render(PlayerState *ps);
void  overlay_render_idle(PlayerState *ps);
void  overlay_cleanup(void);

/* ── Logging API (log.c) ───────────────────────────────────────────── */

void  log_init(void);
void  log_close(void);
void  log_msg(const char *fmt, ...);

/* ── Utility ──────────────────────────────────────────────────────── */

static inline double get_time_sec(void) {
    return (double)av_gettime_relative() / 1000000.0;
}

#endif /* DSVP_H */
