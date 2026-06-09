/**
 * Audio fingerprinting using Chromaprint.
 *
 * Generates AcoustID fingerprints from audio files using FFmpeg for decoding
 * and Chromaprint for fingerprint calculation.
 */

#define G_LOG_DOMAIN "quadrature"

#include "internal.h"
#include <chromaprint.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <string.h>

// Chromaprint expects 16-bit signed samples at specific sample rate
#define CHROMAPRINT_SAMPLE_RATE 11025
#define CHROMAPRINT_CHANNELS    1

quadrature_result_t
mb_fingerprint_generate(const char *audio_path, mb_fingerprint_t *fingerprint)
{
    if (!audio_path || !fingerprint) {
        return QUADRATURE_ERROR_INVALID_PARAM;
    }

    memset(fingerprint, 0, sizeof(mb_fingerprint_t));

    quadrature_result_t result = QUADRATURE_ERROR_INTERNAL;
    AVFormatContext *fmt_ctx = NULL;
    AVCodecContext *codec_ctx = NULL;
    SwrContext *swr_ctx = NULL;
    AVPacket *pkt = NULL;
    AVFrame *frame = NULL;
    ChromaprintContext *chromaprint_ctx = NULL;
    int16_t *resample_buffer = NULL;

    // Open input file with minimal probing — we only need the first audio stream.
    // Default probesize (5 MB) and analyzeduration (5s) cause excessive I/O on
    // network drives. 32 KB + 100ms is sufficient for all common audio formats.
    AVDictionary *open_opts = NULL;
    av_dict_set(&open_opts, "probesize", "32768", 0);
    av_dict_set(&open_opts, "analyzeduration", "100000", 0);

    if (avformat_open_input(&fmt_ctx, audio_path, NULL, &open_opts) < 0) {
        g_warning("Failed to open audio file: %s", audio_path);
        av_dict_free(&open_opts);
        result = QUADRATURE_ERROR_FILE_NOT_FOUND;
        goto cleanup;
    }
    av_dict_free(&open_opts);

    // Find stream info — limit to 1 stream (we only care about audio)
    fmt_ctx->max_streams = 1;
    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        g_warning("Failed to find stream info: %s", audio_path);
        goto cleanup;
    }

    // Find audio stream
    int audio_stream = -1;
    for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audio_stream = i;
            break;
        }
    }

    if (audio_stream < 0) {
        g_warning("No audio stream found: %s", audio_path);
        result = QUADRATURE_ERROR_UNSUPPORTED_FORMAT;
        goto cleanup;
    }

    AVStream *stream = fmt_ctx->streams[audio_stream];
    AVCodecParameters *codecpar = stream->codecpar;

    // Find decoder
    const AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        g_warning("Unsupported codec: %s", audio_path);
        result = QUADRATURE_ERROR_UNSUPPORTED_FORMAT;
        goto cleanup;
    }

    // Create codec context
    codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        g_warning("Failed to allocate codec context");
        result = QUADRATURE_ERROR_OUT_OF_MEMORY;
        goto cleanup;
    }

    if (avcodec_parameters_to_context(codec_ctx, codecpar) < 0) {
        g_warning("Failed to copy codec parameters");
        goto cleanup;
    }

    if (avcodec_open2(codec_ctx, codec, NULL) < 0) {
        g_warning("Failed to open codec: %s", audio_path);
        goto cleanup;
    }

    // Get channel layout
    AVChannelLayout src_ch_layout;
    if (codec_ctx->ch_layout.nb_channels > 0) {
        av_channel_layout_copy(&src_ch_layout, &codec_ctx->ch_layout);
    } else {
        av_channel_layout_default(
            &src_ch_layout,
            codecpar->ch_layout.nb_channels > 0 ? codecpar->ch_layout.nb_channels : 2);
    }

    // Setup resampler to convert to mono 11025Hz 16-bit
    AVChannelLayout dst_ch_layout = AV_CHANNEL_LAYOUT_MONO;

    if (swr_alloc_set_opts2(&swr_ctx,
                            &dst_ch_layout,
                            AV_SAMPLE_FMT_S16,
                            CHROMAPRINT_SAMPLE_RATE,
                            &src_ch_layout,
                            codec_ctx->sample_fmt,
                            codec_ctx->sample_rate,
                            0,
                            NULL)
        < 0) {
        g_warning("Failed to set resampler options");
        av_channel_layout_uninit(&src_ch_layout);
        goto cleanup;
    }

    av_channel_layout_uninit(&src_ch_layout);

    if (swr_init(swr_ctx) < 0) {
        g_warning("Failed to initialize resampler");
        goto cleanup;
    }

    // Allocate packet and frame
    pkt = av_packet_alloc();
    frame = av_frame_alloc();
    if (!pkt || !frame) {
        g_warning("Failed to allocate packet/frame");
        result = QUADRATURE_ERROR_OUT_OF_MEMORY;
        goto cleanup;
    }

    // Create Chromaprint context
    chromaprint_ctx = chromaprint_new(CHROMAPRINT_ALGORITHM_DEFAULT);
    if (!chromaprint_ctx) {
        g_warning("Failed to create Chromaprint context");
        result = QUADRATURE_ERROR_OUT_OF_MEMORY;
        goto cleanup;
    }

    if (!chromaprint_start(chromaprint_ctx, CHROMAPRINT_SAMPLE_RATE, CHROMAPRINT_CHANNELS)) {
        g_warning("Failed to start Chromaprint");
        goto cleanup;
    }

    // Calculate max samples to process (MB_FINGERPRINT_DURATION seconds)
    int64_t max_samples = (int64_t)MB_FINGERPRINT_DURATION * CHROMAPRINT_SAMPLE_RATE;
    int64_t total_samples = 0;

    // Allocate resample buffer (generous size)
    int resample_buffer_size = 8192;
    resample_buffer = g_new(int16_t, resample_buffer_size);

    // Decode and fingerprint
    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index != audio_stream) {
            av_packet_unref(pkt);
            continue;
        }

        if (avcodec_send_packet(codec_ctx, pkt) < 0) {
            av_packet_unref(pkt);
            continue;
        }

        while (avcodec_receive_frame(codec_ctx, frame) >= 0) {
            // Calculate output samples
            int out_samples
                = av_rescale_rnd(swr_get_delay(swr_ctx, codec_ctx->sample_rate) + frame->nb_samples,
                                 CHROMAPRINT_SAMPLE_RATE,
                                 codec_ctx->sample_rate,
                                 AV_ROUND_UP);

            // Resize buffer if needed
            if (out_samples > resample_buffer_size) {
                resample_buffer_size = out_samples;
                resample_buffer = g_renew(int16_t, resample_buffer, resample_buffer_size);
            }

            // Resample
            uint8_t *out_buf = (uint8_t *)resample_buffer;
            int resampled = swr_convert(swr_ctx,
                                        &out_buf,
                                        out_samples,
                                        (const uint8_t **)frame->extended_data,
                                        frame->nb_samples);

            if (resampled > 0) {
                if (!chromaprint_feed(chromaprint_ctx, resample_buffer, resampled)) {
                    g_warning("Chromaprint feed failed");
                    av_frame_unref(frame);
                    av_packet_unref(pkt);
                    goto cleanup;
                }
                total_samples += resampled;
            }

            av_frame_unref(frame);

            // Stop after enough samples
            if (total_samples >= max_samples) {
                break;
            }
        }

        av_packet_unref(pkt);

        if (total_samples >= max_samples) {
            break;
        }
    }

    // Flush resampler
    int out_samples = swr_get_delay(swr_ctx, CHROMAPRINT_SAMPLE_RATE);
    if (out_samples > 0) {
        if (out_samples > resample_buffer_size) {
            resample_buffer_size = out_samples;
            resample_buffer = g_renew(int16_t, resample_buffer, resample_buffer_size);
        }
        uint8_t *out_buf = (uint8_t *)resample_buffer;
        int flushed = swr_convert(swr_ctx, &out_buf, out_samples, NULL, 0);
        if (flushed > 0) {
            chromaprint_feed(chromaprint_ctx, resample_buffer, flushed);
            total_samples += flushed;
        }
    }

    // Finish fingerprinting
    if (!chromaprint_finish(chromaprint_ctx)) {
        g_warning("Chromaprint finish failed");
        goto cleanup;
    }

    // Get fingerprint
    char *fp_str = NULL;
    if (!chromaprint_get_fingerprint(chromaprint_ctx, &fp_str)) {
        g_warning("Failed to get fingerprint");
        goto cleanup;
    }

    fingerprint->fingerprint = g_strdup(fp_str);
    chromaprint_dealloc(fp_str);

    // Calculate duration in seconds
    fingerprint->duration = (int)(total_samples / CHROMAPRINT_SAMPLE_RATE);

    result = QUADRATURE_OK;

cleanup:
    g_free(resample_buffer);
    if (chromaprint_ctx)
        chromaprint_free(chromaprint_ctx);
    if (frame)
        av_frame_free(&frame);
    if (pkt)
        av_packet_free(&pkt);
    if (swr_ctx)
        swr_free(&swr_ctx);
    if (codec_ctx)
        avcodec_free_context(&codec_ctx);
    if (fmt_ctx)
        avformat_close_input(&fmt_ctx);

    return result;
}

void
mb_fingerprint_free(mb_fingerprint_t *fp)
{
    if (!fp)
        return;
    g_free(fp->fingerprint);
    memset(fp, 0, sizeof(mb_fingerprint_t));
}
