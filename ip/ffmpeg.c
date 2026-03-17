/*
 * Copyright 2008-2013 Various Authors
 * Copyright 2007 Kevin Ko <kevin.s.ko@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Updated 2026 for ffmpeg 6.0+ API compatibility:
 *   - Removed avcodec_close() (removed in ffmpeg 6.0; avcodec_free_context() suffices)
 *   - Removed av_register_all() (auto-registration removed in ffmpeg 5.0)
 *   - Dropped all legacy #if guards for ffmpeg < 57.48.101 (circa 2017)
 *   - Replaced stack AVPacket + av_new_packet() with av_packet_alloc() / av_packet_free()
 *   - Use ch_layout API (AVChannelLayout) throughout; removed deprecated
 *     channels / channel_layout fields
 *   - Use avcodec_send_packet() / avcodec_receive_frame() decode API throughout
 *   - Minimum supported ffmpeg version: 6.0 (libavcodec major 60)
 */

#include "../ip.h"
#include "../xmalloc.h"
#include "../debug.h"
#include "../utils.h"
#include "../comment.h"
#ifdef HAVE_CONFIG
#include "../config/ffmpeg.h"
#endif

#include <stdio.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>

#ifndef AVCODEC_MAX_AUDIO_FRAME_SIZE
#define AVCODEC_MAX_AUDIO_FRAME_SIZE 192000
#endif

struct ffmpeg_input {
	AVPacket *pkt;
	int stream_index;

	unsigned long curr_size;
	unsigned long curr_duration;
};

struct ffmpeg_output {
	uint8_t *buffer;
	uint8_t *buffer_malloc;
	uint8_t *buffer_pos;	/* current buffer position */
	int buffer_used_len;
};

struct ffmpeg_private {
	AVCodecContext *codec_context;
	AVFormatContext *input_context;
	AVCodec const *codec;
	SwrContext *swr;

	struct ffmpeg_input *input;
	struct ffmpeg_output *output;
};

static struct ffmpeg_input *ffmpeg_input_create(void)
{
	struct ffmpeg_input *input = xnew(struct ffmpeg_input, 1);

	input->pkt = av_packet_alloc();
	if (input->pkt == NULL) {
		free(input);
		return NULL;
	}
	input->curr_size = 0;
	input->curr_duration = 0;
	return input;
}

static void ffmpeg_input_free(struct ffmpeg_input *input)
{
	av_packet_free(&input->pkt);
	free(input);
}

static struct ffmpeg_output *ffmpeg_output_create(void)
{
	struct ffmpeg_output *output = xnew(struct ffmpeg_output, 1);

	output->buffer_malloc = xnew(uint8_t, AVCODEC_MAX_AUDIO_FRAME_SIZE + 15);
	output->buffer = output->buffer_malloc;
	/* align to 16 bytes so avcodec can use SSE/Altivec/etc */
	while ((intptr_t) output->buffer % 16)
		output->buffer += 1;
	output->buffer_pos = output->buffer;
	output->buffer_used_len = 0;
	return output;
}

static void ffmpeg_output_free(struct ffmpeg_output *output)
{
	free(output->buffer_malloc);
	output->buffer_malloc = NULL;
	output->buffer = NULL;
	free(output);
}

static inline void ffmpeg_buffer_flush(struct ffmpeg_output *output)
{
	output->buffer_pos = output->buffer;
	output->buffer_used_len = 0;
}

static void ffmpeg_init(void)
{
	static int inited = 0;

	if (inited != 0)
		return;
	inited = 1;

	/* Suppress ffmpeg log noise; cmus has its own error reporting. */
	av_log_set_level(AV_LOG_QUIET);
}

static int ffmpeg_open(struct input_plugin_data *ip_data)
{
	struct ffmpeg_private *priv;
	int err = 0;
	int i;
	int stream_index = -1;
	int out_sample_rate;
	AVCodec const *codec;
	AVCodecContext *cc = NULL;
	AVFormatContext *ic = NULL;
	AVCodecParameters *cp = NULL;
	SwrContext *swr = NULL;

	ffmpeg_init();

	err = avformat_open_input(&ic, ip_data->filename, NULL, NULL);
	if (err < 0) {
		d_print("avformat_open_input failed: %d\n", err);
		return -IP_ERROR_FILE_FORMAT;
	}

	do {
		err = avformat_find_stream_info(ic, NULL);
		if (err < 0) {
			d_print("unable to find stream info: %d\n", err);
			err = -IP_ERROR_FILE_FORMAT;
			break;
		}

		for (i = 0; i < (int)ic->nb_streams; i++) {
			cp = ic->streams[i]->codecpar;
			if (cp->codec_type == AVMEDIA_TYPE_AUDIO) {
				stream_index = i;
				break;
			}
		}

		if (stream_index == -1) {
			d_print("could not find audio stream\n");
			err = -IP_ERROR_FILE_FORMAT;
			break;
		}

		codec = avcodec_find_decoder(cp->codec_id);
		if (!codec) {
			d_print("codec not found: %d, %s\n", cp->codec_id,
				avcodec_get_name(cp->codec_id));
			err = -IP_ERROR_UNSUPPORTED_FILE_TYPE;
			break;
		}

		cc = avcodec_alloc_context3(codec);
		if (!cc) {
			d_print("avcodec_alloc_context3 failed\n");
			err = -IP_ERROR_INTERNAL;
			break;
		}

		err = avcodec_parameters_to_context(cc, cp);
		if (err < 0) {
			d_print("avcodec_parameters_to_context failed: %d\n", err);
			err = -IP_ERROR_INTERNAL;
			break;
		}

		if (avcodec_open2(cc, codec, NULL) < 0) {
			d_print("could not open codec: %d, %s\n", cc->codec_id,
				avcodec_get_name(cc->codec_id));
			err = -IP_ERROR_UNSUPPORTED_FILE_TYPE;
			break;
		}

		/* We assume below that no more errors follow. */
	} while (0);

	if (err < 0) {
		/* cc is never successfully opened at this point. */
		avcodec_free_context(&cc);
		avformat_close_input(&ic);
		return err;
	}

	priv = xnew(struct ffmpeg_private, 1);
	priv->codec_context = cc;
	priv->input_context = ic;
	priv->codec = codec;
	priv->input = ffmpeg_input_create();
	if (priv->input == NULL) {
		avcodec_free_context(&cc);
		avformat_close_input(&ic);
		free(priv);
		return -IP_ERROR_INTERNAL;
	}
	priv->input->stream_index = stream_index;
	priv->output = ffmpeg_output_create();

	/* Ensure channel layout is set (some codecs leave it unspecified). */
	if (cc->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
		av_channel_layout_default(&cc->ch_layout, cc->ch_layout.nb_channels);

	/* Prepare for resampling. */
	out_sample_rate = min_u(cc->sample_rate, 384000);
	swr = swr_alloc();
	av_opt_set_chlayout(swr, "in_chlayout",  &cc->ch_layout, 0);
	av_opt_set_chlayout(swr, "out_chlayout", &cc->ch_layout, 0);
	av_opt_set_int(swr, "in_sample_rate",    cc->sample_rate, 0);
	av_opt_set_int(swr, "out_sample_rate",   out_sample_rate, 0);
	av_opt_set_sample_fmt(swr, "in_sample_fmt", cc->sample_fmt, 0);
	priv->swr = swr;

	ip_data->private = priv;
	ip_data->sf = sf_rate(out_sample_rate) | sf_channels(cc->ch_layout.nb_channels);

	switch (cc->sample_fmt) {
	case AV_SAMPLE_FMT_U8:
		ip_data->sf |= sf_bits(8) | sf_signed(0);
		av_opt_set_sample_fmt(swr, "out_sample_fmt", AV_SAMPLE_FMT_U8, 0);
		break;
	case AV_SAMPLE_FMT_S32:
		ip_data->sf |= sf_bits(32) | sf_signed(1);
		av_opt_set_sample_fmt(swr, "out_sample_fmt", AV_SAMPLE_FMT_S32, 0);
		break;
	/* Default: AV_SAMPLE_FMT_S16 */
	default:
		ip_data->sf |= sf_bits(16) | sf_signed(1);
		av_opt_set_sample_fmt(swr, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);
		break;
	}

	swr_init(swr);
	ip_data->sf |= sf_host_endian();
	channel_map_init_waveex(cc->ch_layout.nb_channels, cc->ch_layout.u.mask,
				ip_data->channel_map);
	return 0;
}

static int ffmpeg_close(struct input_plugin_data *ip_data)
{
	struct ffmpeg_private *priv = ip_data->private;

	/* avcodec_free_context() releases all codec resources; avcodec_close()
	 * is not needed and was removed in ffmpeg 6.0. */
	avcodec_free_context(&priv->codec_context);
	avformat_close_input(&priv->input_context);
	swr_free(&priv->swr);
	ffmpeg_input_free(priv->input);
	ffmpeg_output_free(priv->output);
	free(priv);
	ip_data->private = NULL;
	return 0;
}

/*
 * Read and decode packets until a frame is produced.
 * Returns the number of bytes written to output->buffer, 0 on EOF, < 0 on error.
 */
static int ffmpeg_fill_buffer(struct input_plugin_data *ip_data,
			      AVFormatContext *ic, AVCodecContext *cc,
			      struct ffmpeg_input *input,
			      struct ffmpeg_output *output,
			      SwrContext *swr)
{
	AVFrame *frame = av_frame_alloc();
	AVPacket *avpkt = av_packet_alloc();
	int ret = 0;

	if (!frame || !avpkt) {
		av_frame_free(&frame);
		av_packet_free(&avpkt);
		return -IP_ERROR_INTERNAL;
	}

	while (1) {
		/* Try to receive a decoded frame first (may be buffered). */
		ret = avcodec_receive_frame(cc, frame);
		if (ret == 0) {
			/* Got a frame — resample and fill output buffer. */
			int res = swr_convert(swr,
					      &output->buffer,
					      frame->nb_samples,
					      (const uint8_t **)frame->extended_data,
					      frame->nb_samples);
			if (res < 0)
				res = 0;
			output->buffer_pos = output->buffer;
			output->buffer_used_len = res
				* cc->ch_layout.nb_channels
				* sf_get_sample_size(ip_data->sf);
			av_frame_free(&frame);
			av_packet_free(&avpkt);
			return output->buffer_used_len;
		}

		if (ret != AVERROR(EAGAIN)) {
			/* Real decode error or EOF from codec. */
			av_frame_free(&frame);
			av_packet_free(&avpkt);
			return (ret == AVERROR_EOF) ? 0 : -IP_ERROR_INTERNAL;
		}

		/* Codec needs more input — read the next packet. */
		av_packet_unref(avpkt);
		if (av_read_frame(ic, avpkt) < 0) {
			/* Flush the codec so buffered frames are returned. */
			avcodec_send_packet(cc, NULL);
			av_frame_free(&frame);
			av_packet_free(&avpkt);
			return 0;
		}

		if (avpkt->stream_index != input->stream_index) {
			/* Not our audio stream — skip. */
			av_packet_unref(avpkt);
			continue;
		}

		input->curr_size     += avpkt->size;
		input->curr_duration += avpkt->duration;

		ret = avcodec_send_packet(cc, avpkt);
		if (ret < 0 && ret != AVERROR(EAGAIN)) {
			char errstr[AV_ERROR_MAX_STRING_SIZE];
			av_strerror(ret, errstr, sizeof(errstr));
			d_print("avcodec_send_packet() failed: %s\n", errstr);
			av_frame_free(&frame);
			av_packet_free(&avpkt);
			return -IP_ERROR_INTERNAL;
		}
	}

	/* unreachable */
	return -IP_ERROR_INTERNAL;
}

static int ffmpeg_read(struct input_plugin_data *ip_data, char *buffer, int count)
{
	struct ffmpeg_private *priv = ip_data->private;
	struct ffmpeg_output *output = priv->output;
	int rc;
	int out_size;

	if (output->buffer_used_len == 0) {
		rc = ffmpeg_fill_buffer(ip_data,
					priv->input_context,
					priv->codec_context,
					priv->input,
					priv->output,
					priv->swr);
		if (rc <= 0)
			return rc;
	}
	out_size = min_i(output->buffer_used_len, count);
	memcpy(buffer, output->buffer_pos, out_size);
	output->buffer_used_len -= out_size;
	output->buffer_pos += out_size;
	return out_size;
}

static int ffmpeg_seek(struct input_plugin_data *ip_data, double offset)
{
	struct ffmpeg_private *priv = ip_data->private;
	AVStream *st = priv->input_context->streams[priv->input->stream_index];
	int ret;

	int64_t pts = av_rescale_q((int64_t)(offset * AV_TIME_BASE),
				   AV_TIME_BASE_Q, st->time_base);

	avcodec_flush_buffers(priv->codec_context);
	ret = av_seek_frame(priv->input_context, priv->input->stream_index, pts, 0);

	if (ret < 0)
		return -IP_ERROR_FUNCTION_NOT_SUPPORTED;

	ffmpeg_buffer_flush(priv->output);
	return 0;
}

static void ffmpeg_read_metadata(struct growing_keyvals *c, AVDictionary *metadata)
{
	AVDictionaryEntry *tag = NULL;

	while ((tag = av_dict_get(metadata, "", tag, AV_DICT_IGNORE_SUFFIX))) {
		if (tag->value[0])
			comments_add_const(c, tag->key, tag->value);
	}
}

static int ffmpeg_read_comments(struct input_plugin_data *ip_data,
				struct keyval **comments)
{
	struct ffmpeg_private *priv = ip_data->private;
	AVFormatContext *ic = priv->input_context;

	GROWING_KEYVALS(c);

	ffmpeg_read_metadata(&c, ic->metadata);
	for (unsigned i = 0; i < ic->nb_streams; i++)
		ffmpeg_read_metadata(&c, ic->streams[i]->metadata);

	keyvals_terminate(&c);
	*comments = c.keyvals;
	return 0;
}

static int ffmpeg_duration(struct input_plugin_data *ip_data)
{
	struct ffmpeg_private *priv = ip_data->private;
	return (int)(priv->input_context->duration / AV_TIME_BASE);
}

static long ffmpeg_bitrate(struct input_plugin_data *ip_data)
{
	struct ffmpeg_private *priv = ip_data->private;
	long bitrate = priv->input_context->bit_rate;
	return bitrate ? bitrate : -IP_ERROR_FUNCTION_NOT_SUPPORTED;
}

static long ffmpeg_current_bitrate(struct input_plugin_data *ip_data)
{
	struct ffmpeg_private *priv = ip_data->private;
	AVStream *st = priv->input_context->streams[priv->input->stream_index];
	long bitrate = -1;

	/* APE codec returns unreliable bitrate numbers. */
	if (priv->codec->id == AV_CODEC_ID_APE)
		return -1;

	if (priv->input->curr_duration > 0) {
		double seconds = priv->input->curr_duration * av_q2d(st->time_base);
		bitrate = (long)((8 * priv->input->curr_size) / seconds);
		priv->input->curr_size = 0;
		priv->input->curr_duration = 0;
	}
	return bitrate;
}

static char *ffmpeg_codec(struct input_plugin_data *ip_data)
{
	struct ffmpeg_private *priv = ip_data->private;
	return xstrdup(priv->codec->name);
}

static char *ffmpeg_codec_profile(struct input_plugin_data *ip_data)
{
	struct ffmpeg_private *priv = ip_data->private;
	const char *profile = av_get_profile_name(priv->codec,
						   priv->codec_context->profile);
	return profile ? xstrdup(profile) : NULL;
}

const struct input_plugin_ops ip_ops = {
	.open           = ffmpeg_open,
	.close          = ffmpeg_close,
	.read           = ffmpeg_read,
	.seek           = ffmpeg_seek,
	.read_comments  = ffmpeg_read_comments,
	.duration       = ffmpeg_duration,
	.bitrate        = ffmpeg_bitrate,
	.bitrate_current = ffmpeg_current_bitrate,
	.codec          = ffmpeg_codec,
	.codec_profile  = ffmpeg_codec_profile
};

const int ip_priority = 30;
const char *const ip_extensions[] = {
	"aa", "aac", "ac3", "aif", "aifc", "aiff", "ape", "au", "dsf", "fla",
	"flac", "m4a", "m4b", "mka", "mkv", "mp+", "mp2", "mp3", "mp4", "mpc",
	"mpp", "ogg", "opus", "shn", "tak", "tta", "wav", "webm", "wma", "wv",
#ifdef USE_FALLBACK_IP
	"*",
#endif
	NULL
};
const char *const ip_mime_types[]            = { NULL };
const struct input_plugin_opt ip_options[]   = { { NULL } };
const unsigned ip_abi_version                = IP_ABI_VERSION;
