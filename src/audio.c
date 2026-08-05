/*
 * DSVP — Dead Simple Video Player
 * audio.c — Audio decode, resample, and SDL3 audio stream
 *
 * SDL3 audio model:
 *
 *   1. We open an SDL_AudioStream via SDL_OpenAudioDeviceStream(),
 *      which creates a stream bound to a playback device.
 *   2. A "get" callback fires when the device needs more samples.
 *      We decode FFmpeg audio frames, resample to F32 stereo, and
 *      push data into the stream via SDL_PutAudioStreamData().
 *   3. Volume is controlled via SDL_SetAudioStreamGain() — no
 *      manual mixing needed.
 *   4. audio_clock tracks playback position for A/V sync.
 */

#include "dsvp.h"

/* ═══════════════════════════════════════════════════════════════════
 * Audio Decode
 * ═══════════════════════════════════════════════════════════════════ */

int audio_decode_frame(PlayerState *ps) {
    AVPacket pkt;
    int ret;
    int data_size;

    /* Codec can be NULL mid-session if audio_cycle() failed to open the
     * next track — the device stays open, so the callback still fires.
     * Produce silence instead of dereferencing NULL. */
    if (!ps->audio_codec_ctx) return -1;

    for (;;) {
        ret = avcodec_receive_frame(ps->audio_codec_ctx, ps->audio_frame);
        if (ret == 0) {
            /* ── Post-seek stale-frame skip ──
             *
             * After a seek in MPEG-TS, the demuxer reads packets in stream
             * order.  Audio packets interleaved before the video keyframe
             * enter the audio queue with PTS well below the first video
             * frame.  Seek recovery resets audio_clock to video_clock, but
             * the next audio decode would overwrite audio_clock backward,
             * creating multi-second positive A/V drift.
             *
             * Fix: discard decoded audio whose PTS is more than 50ms before
             * the recovery point.  The 50ms tolerance absorbs normal
             * interleave jitter without rejecting valid frames.  The floor
             * clears itself on the first accepted frame. */
            if (ps->audio_pts_floor > 0.0) {
                int64_t fp = ps->audio_frame->best_effort_timestamp;
                if (fp == AV_NOPTS_VALUE) fp = ps->audio_frame->pts;
                if (fp != AV_NOPTS_VALUE) {
                    AVStream *as = ps->fmt_ctx->streams[ps->audio_stream_idx];
                    double pts_sec = (double)fp * av_q2d(as->time_base);
                    if (pts_sec < ps->audio_pts_floor - 0.05) {
                        av_frame_unref(ps->audio_frame);
                        continue;   /* skip stale frame, pull next */
                    }
                }
                ps->audio_pts_floor = 0.0;  /* floor satisfied — clear */
            }

            /* Rebuild the resampler if the stream's format changed under
             * us (ffplay does this per frame). Feeding a 6-channel-planar-
             * configured swr a frame that downgraded to stereo reads
             * frame->extended_data[2..5] == NULL → crash; a stale sample
             * rate pitch-shifts and corrupts the clock increment. */
            if (ps->swr_ctx &&
                (ps->swr_in_format != ps->audio_frame->format ||
                 ps->swr_in_rate   != ps->audio_frame->sample_rate ||
                 av_channel_layout_compare(&ps->swr_in_layout,
                                           &ps->audio_frame->ch_layout))) {
                log_msg("Audio: input format changed mid-stream — rebuilding resampler");
                swr_free(&ps->swr_ctx);
            }

            if (!ps->swr_ctx) {
                AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
                ret = swr_alloc_set_opts2(&ps->swr_ctx,
                    &out_layout, AV_SAMPLE_FMT_FLT, ps->audio_spec.freq,    // was AV_SAMPLE_FMT_S16
                    &ps->audio_frame->ch_layout, ps->audio_frame->format,
                    ps->audio_frame->sample_rate, 0, NULL);
                if (ret < 0 || swr_init(ps->swr_ctx) < 0) {
                    log_msg("ERROR: swr init failed: %s", av_err2str(ret));
                    /* Free the half-built context — a non-NULL but
                     * uninitialized swr_ctx skipped this branch forever
                     * and silenced the track for the whole file. */
                    swr_free(&ps->swr_ctx);
                    return -1;
                }
                av_channel_layout_copy(&ps->swr_in_layout,
                                       &ps->audio_frame->ch_layout);
                ps->swr_in_format = ps->audio_frame->format;
                ps->swr_in_rate   = ps->audio_frame->sample_rate;
            }

            int out_samples = swr_get_out_samples(ps->swr_ctx, ps->audio_frame->nb_samples);
            if (out_samples < 0) return -1;
            int out_size = out_samples * 2 * 4;

            /* HEAP-OVERFLOW FIX: the old code re-malloc'd the SAME
             * AUDIO_BUF_SIZE when out_size exceeded it, then told
             * swr_convert the buffer held out_samples anyway — a large
             * decoded frame plus heavy upsampling could write past the
             * end of the allocation. Grow the buffer to fit instead. */
            unsigned int need = (out_size > AUDIO_BUF_SIZE)
                              ? (unsigned int)out_size
                              : (unsigned int)AUDIO_BUF_SIZE;
            if (!ps->audio_buf || need > ps->audio_buf_cap) {
                av_free(ps->audio_buf);
                ps->audio_buf = av_malloc(need);
                if (!ps->audio_buf) { ps->audio_buf_cap = 0; return -1; }
                ps->audio_buf_cap = need;
            }

            uint8_t *out_buf = ps->audio_buf;
            /* extended_data, not data: for planar layouts above 8 channels
             * data[] holds only the first AV_NUM_DATA_POINTERS plane
             * pointers — indexing past it walks into AVFrame struct
             * memory interpreted as pointers. */
            int converted = swr_convert(ps->swr_ctx,
                &out_buf, out_samples,
                (const uint8_t **)ps->audio_frame->extended_data,
                ps->audio_frame->nb_samples);

            if (converted < 0) {
                fprintf(stderr, "[DSVP] Resample error\n");
                return -1;
            }

            data_size = converted * 2 * 4;

            int64_t frame_pts = ps->audio_frame->best_effort_timestamp;
            if (frame_pts == AV_NOPTS_VALUE)
                frame_pts = ps->audio_frame->pts;
            if (frame_pts == AV_NOPTS_VALUE)
               log_msg("WARN: audio frame with no PTS (audio_clock=%.3f)", ps->audio_clock);
            if (frame_pts != AV_NOPTS_VALUE) {
                AVStream *as = ps->fmt_ctx->streams[ps->audio_stream_idx];
                ps->audio_clock = (double)frame_pts * av_q2d(as->time_base);
            }
            ps->audio_clock += (double)converted / ps->audio_spec.freq;

            av_frame_unref(ps->audio_frame);
            return data_size;
        }

        if (ret != AVERROR(EAGAIN))
            return -1;

        ret = pq_get(&ps->audio_pq, &pkt, 0);
        if (ret <= 0) return -1;

        ret = avcodec_send_packet(ps->audio_codec_ctx, &pkt);
        av_packet_unref(&pkt);
        if (ret < 0) return -1;
    }
}


/* ═══════════════════════════════════════════════════════════════════
 * SDL3 Audio Stream Callback
 * ═══════════════════════════════════════════════════════════════════ */

void SDLCALL audio_callback(void *userdata, SDL_AudioStream *stream,
                             int additional_amount, int total_amount) {
    PlayerState *ps = (PlayerState *)userdata;
    (void)total_amount;

    if (ps->paused || ps->seek_request || ps->seeking) return;
    if (additional_amount <= 0) return;

    int written = 0;
    while (written < additional_amount) {
        if (ps->audio_buf_index >= ps->audio_buf_size) {
            int decoded = audio_decode_frame(ps);
            if (decoded <= 0) break;
            ps->audio_buf_size  = decoded;
            ps->audio_buf_index = 0;
        }

        int remaining = ps->audio_buf_size - ps->audio_buf_index;
        int to_push   = additional_amount - written;
        if (to_push > remaining) to_push = remaining;

        SDL_PutAudioStreamData(stream,
            ps->audio_buf + ps->audio_buf_index, to_push);

        written             += to_push;
        ps->audio_buf_index += to_push;
    }

    /* ── Audio clock sync snapshot ──
     *
     * audio_clock reflects the PTS at the END of the last decoded frame.
     * But audio_decode_frame() updates audio_clock multiple times during
     * the callback (line 70: set PTS, line 72: += samples, loop repeats).
     * The main thread reads audio_clock for A/V sync at arbitrary times.
     *
     * If the main thread reads DURING this callback, it sees the raw
     * decode position (too far ahead) instead of the corrected playback
     * position.  This data race causes audio_clock to appear ~20-40ms
     * ahead, making av_diff chronically negative and locking 60fps
     * content into a two-decode-per-VSync equilibrium.
     *
     * Fix: compute the corrected value ONCE at the end of the callback
     * and write it to audio_clock_sync.  The main thread reads ONLY
     * audio_clock_sync, which always has the full correction applied.
     *
     * CRITICAL: Cap the correction at 100ms to prevent FLAC/large-buffer
     * runaway where SDL reports huge queued amounts during startup. */
    if (ps->audio_spec.freq > 0 && !ps->seek_recovering) {
        int bytes_per_sample = 2 * 4;  /* F32 stereo */

        /* Our internal buffer: decoded but not yet pushed to SDL */
        int internal_pending = ps->audio_buf_size - ps->audio_buf_index;
        if (internal_pending < 0) internal_pending = 0;

        /* SDL stream pipeline: pushed but not yet played by device */
        int stream_pending = SDL_GetAudioStreamQueued(stream);
        if (stream_pending < 0) stream_pending = 0;

        double buffered_sec = (double)(internal_pending + stream_pending)
                            / (ps->audio_spec.freq * bytes_per_sample);

        /* Cap at 100ms — prevents FLAC/large-buffer runaway */
        if (buffered_sec > 0.1) buffered_sec = 0.1;

        /* Aligned 64-bit write — atomic on x86-64 and ARMv8. Not guaranteed portable. */
        ps->audio_clock_sync = ps->audio_clock - buffered_sec;
    } else {
        ps->audio_clock_sync = ps->audio_clock;
    }
}


/* ═══════════════════════════════════════════════════════════════════
 * Open / Close Audio Device
 * ═══════════════════════════════════════════════════════════════════ */

int audio_open(PlayerState *ps) {
    if (!ps->audio_codec_ctx) return -1;

    /* ── Probe HDMI/DP sink capabilities if bitstream mode requested ──
     * Reprobe on file open when the previous probe found no usable sink
     * — covers the HDMI cable being plugged in after launch. A probe
     * that found capabilities persists for the session as before. */
    int caps_found = ps->bitstream_caps.support_ac3
                  || ps->bitstream_caps.support_eac3
                  || ps->bitstream_caps.support_truehd
                  || ps->bitstream_caps.support_dts
                  || ps->bitstream_caps.support_dtshd;
    if (ps->audio_mode != AUDIO_MODE_PCM
            && (!ps->bitstream_caps.probed || !caps_found)) {
        if (bitstream_probe(&ps->bitstream_caps) == 0) {
            /* Check if the current audio codec can pass through */
            enum AVCodecID codec = ps->audio_codec_ctx->codec_id;
            if (bitstream_can_passthrough(&ps->bitstream_caps, codec)) {
                log_msg("Audio: codec %s is passthrough-eligible",
                        avcodec_get_name(codec));
                /* Phase 3 TODO: open SPDIF muxer + IEC958 device here.
                 * For now, fall through to PCM — probe data is logged
                 * and visible in the debug overlay. */
            }
        }
    }

    SDL_AudioSpec spec;
    SDL_zero(spec);
    spec.format   = SDL_AUDIO_F32;    // was SDL_AUDIO_S16
    spec.channels = 2;
    spec.freq     = ps->audio_codec_ctx->sample_rate;

    ps->audio_stream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
        &spec, audio_callback, ps);

    if (!ps->audio_stream) {
        log_msg("ERROR: SDL_OpenAudioDeviceStream failed: %s", SDL_GetError());
        return -1;
    }

    ps->audio_spec = spec;

    /* Free any existing buffer first — the sample-rate-change reopen in
     * audio_cycle() previously leaked one allocation per reopen. */
    if (ps->audio_buf) av_free(ps->audio_buf);
    ps->audio_buf       = av_malloc(AUDIO_BUF_SIZE);
    ps->audio_buf_cap   = ps->audio_buf ? AUDIO_BUF_SIZE : 0;
    ps->audio_buf_size  = 0;
    ps->audio_buf_index = 0;

    SDL_SetAudioStreamGain(ps->audio_stream, ps->volume);
    /* Audio device starts paused. Resume is deferred until the first
     * video frame is displayed (seek_recovering gate in main.c).
     * This prevents audio from running ahead during initial decode latency. */

    log_msg("Audio opened: %s %d Hz, %d ch (SDL3 stream)",
        (spec.format == SDL_AUDIO_F32) ? "F32" : "S16",
        spec.freq, spec.channels);
    return 0;
}

void audio_close(PlayerState *ps) {
    if (ps->audio_stream) {
        SDL_DestroyAudioStream(ps->audio_stream);
        ps->audio_stream = NULL;
    }
    /* Invalidate the remembered device rate. audio_cycle's reopen
     * decision compares the new track's rate against this — a stale
     * value from a dead stream let the same-rate case skip the reopen
     * entirely, setting audio_stream_idx with no device and no
     * callback: demux refilled audio_pq, nothing drained it, and the
     * queue-full throttle froze all playback (the resurrected-C3
     * finding of the final review). */
    ps->audio_spec.freq = 0;
}


/* ═══════════════════════════════════════════════════════════════════
 * Audio Stream Discovery
 * ═══════════════════════════════════════════════════════════════════ */

void audio_find_streams(PlayerState *ps) {
    ps->aud_count     = 0;
    ps->aud_selection = 0;

    for (unsigned i = 0; i < ps->fmt_ctx->nb_streams && ps->aud_count < MAX_AUDIO_STREAMS; i++) {
        AVStream *st = ps->fmt_ctx->streams[i];
        if (st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;

        int idx = ps->aud_count;
        ps->aud_stream_indices[idx] = (int)i;

        const AVDictionaryEntry *lang  = av_dict_get(st->metadata, "language", NULL, 0);
        const AVDictionaryEntry *title = av_dict_get(st->metadata, "title", NULL, 0);
        const char *codec_name = avcodec_get_name(st->codecpar->codec_id);
        int channels = st->codecpar->ch_layout.nb_channels;
        int rate     = st->codecpar->sample_rate;

        char desc[128] = {0};
        if (title && lang)
            snprintf(desc, sizeof(desc), "%s (%s)", title->value, lang->value);
        else if (lang)
            snprintf(desc, sizeof(desc), "%s", lang->value);
        else if (title)
            snprintf(desc, sizeof(desc), "%s", title->value);
        else
            snprintf(desc, sizeof(desc), "Track %d", idx + 1);

        snprintf(ps->aud_stream_names[idx], sizeof(ps->aud_stream_names[idx]),
            "%s [%s %dch %dHz]", desc, codec_name, channels, rate);

        if ((int)i == ps->audio_stream_idx)
            ps->aud_selection = idx;

        log_msg("Audio stream %d: [%d] %s", idx, (int)i, ps->aud_stream_names[idx]);
        ps->aud_count++;
    }

    /* Disambiguate identical display names */
    for (int a = 0; a < ps->aud_count; a++) {
        for (int b = a + 1; b < ps->aud_count; b++) {
            if (strcmp(ps->aud_stream_names[a], ps->aud_stream_names[b]) == 0) {
                char tmp[128];
                snprintf(tmp, sizeof(tmp), "%s #1", ps->aud_stream_names[a]);
                snprintf(ps->aud_stream_names[a], sizeof(ps->aud_stream_names[a]), "%s", tmp);
                snprintf(tmp, sizeof(tmp), "%s #2", ps->aud_stream_names[b]);
                snprintf(ps->aud_stream_names[b], sizeof(ps->aud_stream_names[b]), "%s", tmp);
            }
        }
    }

    log_msg("Found %d audio stream(s), active: %d (%s)",
        ps->aud_count, ps->aud_selection,
        ps->aud_count > 0 ? ps->aud_stream_names[ps->aud_selection] : "none");
}


/* ═══════════════════════════════════════════════════════════════════
 * Audio Track Cycling
 * ═══════════════════════════════════════════════════════════════════ */

void audio_cycle(PlayerState *ps) {
    if (ps->aud_count <= 1) {
        snprintf(ps->aud_osd, sizeof(ps->aud_osd),
            ps->aud_count == 0 ? "No audio tracks" : "Only one audio track");
        ps->aud_osd_until = get_time_sec() + 2.0;
        return;
    }

    int new_sel = (ps->aud_selection + 1) % ps->aud_count;

    /* Skip TrueHD tracks — unusable without HDMI bitstreaming */
    int checked = 0;
    while (checked < ps->aud_count) {
        int idx = ps->aud_stream_indices[new_sel];
        AVStream *st = ps->fmt_ctx->streams[idx];
        if (st->codecpar->codec_id != AV_CODEC_ID_TRUEHD)
            break;
        log_msg("Audio: skipping TrueHD track %d (%s)",
            new_sel, ps->aud_stream_names[new_sel]);
        new_sel = (new_sel + 1) % ps->aud_count;
        checked++;
    }
    if (checked >= ps->aud_count || new_sel == ps->aud_selection) {
        /* All other tracks are TrueHD — stay on current */
        snprintf(ps->aud_osd, sizeof(ps->aud_osd),
            "No other non-TrueHD audio tracks");
        ps->aud_osd_until = get_time_sec() + 2.0;
        return;
    }

    int new_stream_idx = ps->aud_stream_indices[new_sel];

    log_msg("Audio: switching to %s (stream %d)",
        ps->aud_stream_names[new_sel], new_stream_idx);

    /* The demux thread's seek handler flushes audio_codec_ctx under
     * seek_mutex; freeing/swapping the context here without that lock is a
     * NULL-check-then-flush use-after-free (seek-then-A within the seek
     * window, reachable via key auto-repeat). Same lock order as demux
     * (seek_mutex, then stream lock) — no inversion. */
    SDL_LockMutex(ps->seek_mutex);

    if (ps->audio_stream) {
        SDL_PauseAudioStreamDevice(ps->audio_stream);
        /* Barrier: pause stops FUTURE callbacks but does not wait for
         * one already in flight. SDL runs the get-callback while holding
         * the stream lock, so lock+unlock returns only after any
         * in-flight audio_decode_frame() finishes — only then is it
         * safe to free the codec and swr contexts below. */
        SDL_LockAudioStream(ps->audio_stream);
        SDL_UnlockAudioStream(ps->audio_stream);
    }

    pq_flush(&ps->audio_pq);

    if (ps->audio_codec_ctx)
        avcodec_free_context(&ps->audio_codec_ctx);
    if (ps->swr_ctx)
        swr_free(&ps->swr_ctx);

    ps->audio_buf_size  = 0;
    ps->audio_buf_index = 0;

    AVStream *as = ps->fmt_ctx->streams[new_stream_idx];
    const AVCodec *codec = avcodec_find_decoder(as->codecpar->codec_id);
    if (!codec) {
        log_msg("ERROR: No decoder for audio codec %s",
            avcodec_get_name(as->codecpar->codec_id));
        goto fail_video_only;
    }

    ps->audio_codec_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(ps->audio_codec_ctx, as->codecpar);
    ps->audio_codec_ctx->thread_count = 0;

    int ret = avcodec_open2(ps->audio_codec_ctx, codec, NULL);
    if (ret < 0) {
        log_msg("ERROR: Cannot open audio codec: %s", av_err2str(ret));
        avcodec_free_context(&ps->audio_codec_ctx);
        goto fail_video_only;
    }

    int new_rate = ps->audio_codec_ctx->sample_rate;
    /* Reopen when the rate changed OR when there is no live stream at
     * all (recovering from an earlier audio failure — the whole reason
     * the user is pressing A). Keying on rate alone let the common
     * same-rate case land in the success path with a codec but no
     * device: the total-playback freeze class, resurrected. */
    if (!ps->audio_stream || new_rate != ps->audio_spec.freq) {
        log_msg("Audio: %s (rate %d -> %d)",
            ps->audio_stream ? "sample rate changed, reopening stream"
                             : "no live stream, reopening device",
            ps->audio_spec.freq, new_rate);
        audio_close(ps);
        if (audio_open(ps) < 0) {
            avcodec_free_context(&ps->audio_codec_ctx);
            goto fail_video_only;
        }
    }

    ps->aud_selection    = new_sel;
    ps->audio_stream_idx = new_stream_idx;

    log_msg("Audio: now playing %s (%s %dHz)",
        ps->aud_stream_names[new_sel], codec->name, new_rate);

    /* Recovery seek target: prefer the live video clock — after an
     * audio failure, audio_clock_sync is frozen at the moment audio
     * died and would yank playback back to a stale position. */
    double pos = (ps->video_stream_idx >= 0) ? ps->video_clock
                                             : ps->audio_clock_sync;
    if (pos < 0.1) pos = 0.1;
    ps->seek_target  = (int64_t)(pos * AV_TIME_BASE);
    ps->seek_flags   = AVSEEK_FLAG_BACKWARD;
    ps->seek_request = 1;

    SDL_UnlockMutex(ps->seek_mutex);

    /* No device resume here: the recovery seek just issued keeps audio
     * paused until the first post-seek frame displays (main.c's
     * seek-recovery resume). Resuming here raced the demux seek
     * handler's own pause — a resume after its pause let audio run
     * ahead during recovery. */

    snprintf(ps->aud_osd, sizeof(ps->aud_osd), "Audio: %s",
        ps->aud_stream_names[new_sel]);
    ps->aud_osd_until = get_time_sec() + 2.0;
    return;

fail_video_only:
    /* The old track is already torn down; leaving audio_stream_idx pointing
     * at it would keep demux queueing packets nothing drains — the queue-full
     * throttle then stalls all playback permanently, with the device paused.
     * Drop to video-only on the video clock instead. audio_close() must
     * happen under seek_mutex: the demux seek path touches audio_stream. */
    ps->audio_stream_idx = -1;
    /* Flush AFTER clearing the index: demux routes without seek_mutex,
     * so packets read during the failed codec-open window could land
     * after an earlier flush and sit orphaned forever, blocking the
     * EOF close condition (audio_pq must reach zero). Index first
     * stops new routing; then the flush clears stragglers. */
    pq_flush(&ps->audio_pq);
    audio_close(ps);
    SDL_UnlockMutex(ps->seek_mutex);
    log_msg("Audio: track switch failed — continuing video-only");
    snprintf(ps->aud_osd, sizeof(ps->aud_osd), "Audio: codec error, audio off");
    ps->aud_osd_until = get_time_sec() + 2.0;
}
