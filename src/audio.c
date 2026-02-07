/*
 * DSVP — Dead Simple Video Player
 * audio.c — Audio decode, resample, and SDL audio callback
 *
 * How audio playback works:
 *
 *   1. SDL opens an audio device with a callback function.
 *   2. SDL's audio thread calls audio_callback() whenever it needs
 *      more samples to play.
 *   3. audio_callback() pulls data from an internal buffer. When the
 *      buffer runs out, it calls audio_decode_frame() to decode more
 *      packets from the audio packet queue and resample them to the
 *      output format (signed 16-bit, stereo, device sample rate).
 *   4. As samples are consumed, we update audio_clock to track the
 *      current playback position. The video sync uses this clock
 *      as the master reference.
 */

#include "dsvp.h"

/* ═══════════════════════════════════════════════════════════════════
 * Audio Decode
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * Decode one audio frame from the packet queue and resample it
 * to the SDL output format. Returns the number of bytes of
 * resampled data, or -1 on error / no data.
 */
int audio_decode_frame(PlayerState *ps) {
    AVPacket pkt;
    int ret;
    int data_size = 0;

    for (;;) {
        /* Try to receive a decoded frame */
        ret = avcodec_receive_frame(ps->audio_codec_ctx, ps->audio_frame);
        if (ret == 0) {
            /* Got a frame — resample to output format */

            /* Lazy-init SwrContext on first frame (we need the actual
             * frame's channel layout which may differ from codecpar) */
            if (!ps->swr_ctx) {
                AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;

                ret = swr_alloc_set_opts2(&ps->swr_ctx,
                    &out_layout,                        /* out ch layout  */
                    AV_SAMPLE_FMT_S16,                  /* out format     */
                    ps->audio_spec.freq,                /* out sample rate*/
                    &ps->audio_frame->ch_layout,        /* in ch layout   */
                    ps->audio_frame->format,            /* in format      */
                    ps->audio_frame->sample_rate,       /* in sample rate */
                    0, NULL
                );
            if (ret < 0 || swr_init(ps->swr_ctx) < 0) {
                    log_msg("ERROR: swr init failed: %s", av_err2str(ret));
                    return -1;
                }
            }

            /* Calculate output buffer size */
            int out_samples = swr_get_out_samples(ps->swr_ctx, ps->audio_frame->nb_samples);
            int out_size = out_samples * 2 * 2; /* stereo * 16-bit */

            /* Grow buffer if needed */
            if (!ps->audio_buf || out_size > AUDIO_BUF_SIZE) {
                av_free(ps->audio_buf);
                ps->audio_buf = av_malloc(AUDIO_BUF_SIZE);
                if (!ps->audio_buf) return -1;
            }

            uint8_t *out_buf = ps->audio_buf;
            int converted = swr_convert(ps->swr_ctx,
                &out_buf, out_samples,
                (const uint8_t **)ps->audio_frame->data,
                ps->audio_frame->nb_samples
            );

            if (converted < 0) {
                fprintf(stderr, "[DSVP] Resample error\n");
                return -1;
            }

            data_size = converted * 2 * 2; /* stereo * 16-bit */

            /* Update audio clock from frame PTS */
            if (ps->audio_frame->pts != AV_NOPTS_VALUE) {
                AVStream *as = ps->fmt_ctx->streams[ps->audio_stream_idx];
                ps->audio_clock = (double)ps->audio_frame->pts * av_q2d(as->time_base);
            }
            /* Advance clock by the duration of the samples we just decoded */
            ps->audio_clock += (double)converted / ps->audio_spec.freq;

            av_frame_unref(ps->audio_frame);
            return data_size;
        }

        if (ret != AVERROR(EAGAIN)) {
            return -1; /* decoder error or EOF */
        }

        /* Need more packets — pull from queue (non-blocking) */
        ret = pq_get(&ps->audio_pq, &pkt, 0);
        if (ret <= 0) return -1; /* no packets right now */

        ret = avcodec_send_packet(ps->audio_codec_ctx, &pkt);
        av_packet_unref(&pkt);

        if (ret < 0) return -1;
    }
}


/* ═══════════════════════════════════════════════════════════════════
 * SDL Audio Callback
 * ═══════════════════════════════════════════════════════════════════
 *
 * Called by SDL's audio thread whenever the device needs more samples.
 * We fill `stream` with `len` bytes of audio data, mixing our volume.
 */

void audio_callback(void *userdata, Uint8 *stream, int len) {
    PlayerState *ps = (PlayerState *)userdata;

    /* Silence the buffer first (prevents noise on underrun) */
    memset(stream, 0, len);

    if (ps->paused || ps->seek_request || ps->seeking) return;

    int written = 0;

    while (written < len) {
        /* If our internal buffer is exhausted, decode more */
        if (ps->audio_buf_index >= ps->audio_buf_size) {
            int decoded = audio_decode_frame(ps);
            if (decoded <= 0) {
                /* No data available — output silence for remainder */
                break;
            }
            ps->audio_buf_size  = decoded;
            ps->audio_buf_index = 0;
        }

        /* Copy from our buffer to SDL's buffer */
        int remaining = ps->audio_buf_size - ps->audio_buf_index;
        int to_copy   = len - written;
        if (to_copy > remaining) to_copy = remaining;

        /* Apply volume: mix into the stream buffer */
        SDL_MixAudioFormat(
            stream + written,
            ps->audio_buf + ps->audio_buf_index,
            AUDIO_S16SYS,
            to_copy,
            (int)(ps->volume * SDL_MIX_MAXVOLUME)
        );

        written             += to_copy;
        ps->audio_buf_index += to_copy;
    }
}


/* ═══════════════════════════════════════════════════════════════════
 * Open / Close Audio Device
 * ═══════════════════════════════════════════════════════════════════ */

int audio_open(PlayerState *ps) {
    if (!ps->audio_codec_ctx) return -1;

    SDL_AudioSpec wanted;
    SDL_zero(wanted);

    wanted.freq     = ps->audio_codec_ctx->sample_rate;
    wanted.format   = AUDIO_S16SYS;     /* signed 16-bit, native byte order */
    wanted.channels = 2;                /* always output stereo              */
    wanted.samples  = SDL_AUDIO_BUFFER_SZ;
    wanted.callback = audio_callback;
    wanted.userdata = ps;

    ps->audio_dev = SDL_OpenAudioDevice(NULL, 0, &wanted, &ps->audio_spec, 0);
    if (ps->audio_dev == 0) {
        log_msg("ERROR: SDL_OpenAudioDevice failed: %s", SDL_GetError());
        return -1;
    }

    /* Allocate initial audio buffer */
    ps->audio_buf       = av_malloc(AUDIO_BUF_SIZE);
    ps->audio_buf_size  = 0;
    ps->audio_buf_index = 0;

    /* Start playback */
    SDL_PauseAudioDevice(ps->audio_dev, 0);

    log_msg("Audio opened: %d Hz, %d ch, buffer %d samples",
        ps->audio_spec.freq, ps->audio_spec.channels, ps->audio_spec.samples);

    return 0;
}

void audio_close(PlayerState *ps) {
    if (ps->audio_dev) {
        SDL_CloseAudioDevice(ps->audio_dev);
        ps->audio_dev = 0;
    }
}
