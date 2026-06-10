#define G_LOG_DOMAIN "quadrature"

#include "internal.h"

#include <libavutil/channel_layout.h>
#include <libavutil/log.h>

#include <pthread.h>
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * FFmpeg Decoder
 *
 * Low-level decode + resample to the pipeline's canonical (target channels +
 * rate) interleaved float32 format. Used by audio_cache to decode whole tracks
 * to PCM buffers before playback — there is no streaming path.
 * ═══════════════════════════════════════════════════════════════════════════ */

static pthread_once_t ffmpeg_init_once = PTHREAD_ONCE_INIT;
static void
ffmpeg_init_internal(void)
{
    av_log_set_level(AV_LOG_ERROR);
    avformat_network_init();
    g_message("FFmpeg initialized (%s)", av_version_info());
}

quadrature_result_t
ffmpeg_decoder_open(ffmpeg_decoder_t *dec, const char *path, uint32_t rate, uint32_t channels)
{
    pthread_once(&ffmpeg_init_once, ffmpeg_init_internal);

    memset(dec, 0, sizeof(*dec));
    dec->output_sample_rate = (int)rate;

    int ret;
    const AVCodec *codec = NULL;

    ret = avformat_open_input(&dec->fmt_ctx, path, NULL, NULL);
    if (ret < 0) {
        g_critical("Cannot open %s", path);
        return QUADRATURE_ERROR_FILE_NOT_FOUND;
    }

    ret = avformat_find_stream_info(dec->fmt_ctx, NULL);
    if (ret < 0)
        goto fail;

    dec->stream_index = av_find_best_stream(dec->fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (dec->stream_index < 0)
        goto fail;

    AVStream *stream = dec->fmt_ctx->streams[dec->stream_index];

    dec->codec_ctx = avcodec_alloc_context3(codec);
    if (!dec->codec_ctx)
        goto fail;

    ret = avcodec_parameters_to_context(dec->codec_ctx, stream->codecpar);
    if (ret < 0)
        goto fail;

    ret = avcodec_open2(dec->codec_ctx, codec, NULL);
    if (ret < 0)
        goto fail;

    int source_sample_rate = dec->codec_ctx->sample_rate;
    AVChannelLayout *ch_layout = &dec->codec_ctx->ch_layout;

    /* Resample from the source layout/rate to the canonical interleaved float32
     * layout at the target rate. The output layout is the default for the
     * requested channel count (stereo, 5.1, 7.1, …), so swresample down/upmixes
     * whatever the source provides. */
    AVChannelLayout out_layout;
    av_channel_layout_default(&out_layout, (int)channels);
    ret = swr_alloc_set_opts2(&dec->swr_ctx,
                              &out_layout,
                              AV_SAMPLE_FMT_FLT,
                              dec->output_sample_rate,
                              ch_layout,
                              dec->codec_ctx->sample_fmt,
                              source_sample_rate,
                              0,
                              NULL);
    av_channel_layout_uninit(&out_layout);
    if (ret < 0 || !dec->swr_ctx)
        goto fail;

    ret = swr_init(dec->swr_ctx);
    if (ret < 0)
        goto fail;

    dec->frame = av_frame_alloc();
    dec->packet = av_packet_alloc();
    if (!dec->frame || !dec->packet)
        goto fail;

    dec->time_base = av_q2d(stream->time_base);

    return QUADRATURE_OK;

fail:
    ffmpeg_decoder_close(dec);
    return QUADRATURE_ERROR_UNSUPPORTED_FORMAT;
}

int
ffmpeg_decoder_read(ffmpeg_decoder_t *dec, float *buffer, size_t max_frames)
{
    if (!dec->fmt_ctx || !dec->codec_ctx)
        return -1;

    int ret;

    for (;;) {
        ret = avcodec_receive_frame(dec->codec_ctx, dec->frame);
        if (ret == 0) {
            int out = swr_convert(dec->swr_ctx,
                                  (uint8_t **)&buffer,
                                  (int)max_frames,
                                  (const uint8_t **)dec->frame->extended_data,
                                  dec->frame->nb_samples);
            av_frame_unref(dec->frame);
            if (out < 0)
                return -1;
            return out;
        } else if (ret == AVERROR(EAGAIN)) {
            /* need more packets */
        } else if (ret == AVERROR_EOF) {
            int out = swr_convert(dec->swr_ctx, (uint8_t **)&buffer, (int)max_frames, NULL, 0);
            if (out > 0)
                return out;
            return 0;
        } else {
            return -1;
        }

        ret = av_read_frame(dec->fmt_ctx, dec->packet);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                avcodec_send_packet(dec->codec_ctx, NULL);
                continue;
            }
            return -1;
        }

        if (dec->packet->stream_index != dec->stream_index) {
            av_packet_unref(dec->packet);
            continue;
        }

        ret = avcodec_send_packet(dec->codec_ctx, dec->packet);
        av_packet_unref(dec->packet);
        if (ret < 0 && ret != AVERROR(EAGAIN))
            return -1;
    }
}

void
ffmpeg_decoder_close(ffmpeg_decoder_t *dec)
{
    if (dec->frame) {
        av_frame_free(&dec->frame);
        dec->frame = NULL;
    }
    if (dec->packet) {
        av_packet_free(&dec->packet);
        dec->packet = NULL;
    }
    if (dec->swr_ctx) {
        swr_free(&dec->swr_ctx);
        dec->swr_ctx = NULL;
    }
    if (dec->codec_ctx) {
        avcodec_free_context(&dec->codec_ctx);
        dec->codec_ctx = NULL;
    }
    if (dec->fmt_ctx) {
        avformat_close_input(&dec->fmt_ctx);
        dec->fmt_ctx = NULL;
    }
}

ffmpeg_decoder_metadata_t
ffmpeg_decoder_metadata(const ffmpeg_decoder_t *dec)
{
    ffmpeg_decoder_metadata_t m = { 0 };
    if (!dec->fmt_ctx || !dec->codec_ctx)
        return m;

    AVStream *stream = dec->fmt_ctx->streams[dec->stream_index];

    /* Duration in output frames, preferring the stream's own timestamp. */
    double duration = 0;
    if (stream->duration != AV_NOPTS_VALUE) {
        duration = stream->duration * dec->time_base;
    } else if (dec->fmt_ctx->duration != AV_NOPTS_VALUE) {
        duration = dec->fmt_ctx->duration / (double)AV_TIME_BASE;
    }
    m.duration_frames = (uint64_t)(duration * dec->output_sample_rate);

    /* Codec name is ffmpeg's, not the path's: an .m4a resolves to "aac" or
     * "alac", an .ogg to "vorbis"/"opus", regardless of the filename. */
    g_strlcpy(m.codec_name, avcodec_get_name(dec->codec_ctx->codec_id), sizeof(m.codec_name));

    m.sample_rate = (uint32_t)dec->codec_ctx->sample_rate;
    m.channels = (uint32_t)dec->codec_ctx->ch_layout.nb_channels;
    m.bit_rate = dec->codec_ctx->bit_rate ? dec->codec_ctx->bit_rate : dec->fmt_ctx->bit_rate;
    m.bit_depth
        = dec->codec_ctx->bits_per_raw_sample > 0 ? (uint32_t)dec->codec_ctx->bits_per_raw_sample : 0;

    return m;
}
