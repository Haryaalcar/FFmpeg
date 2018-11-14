/*
 * Fake (dummy) H.264 video decoder, based on reduced OpenH264 decoder code.
 * Copyright (C) 2016 Martin Storsjo
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libavutil/common.h"
#include "libavutil/fifo.h"
#include "libavutil/imgutils.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mathematics.h"
#include "libavutil/opt.h"

#include "avcodec.h"
#include "internal.h"

typedef struct FakeH264DecoderContext {
    uint8_t just_a_zero_buffer_u[640*480];
    uint8_t just_a_zero_buffer_v[320*240];
    uint8_t just_a_zero_buffer_w[320*240];
} FakeH264DecoderContext;

static av_cold int fakeh264_decode_close(AVCodecContext *avctx)
{
    FakeH264DecoderContext *s = avctx->priv_data;
    
    // Here goes cleanup

    return 0;
}

static av_cold int fakeh264_decode_init(AVCodecContext *avctx)
{
    FakeH264DecoderContext *s = avctx->priv_data;

    memset(s->just_a_zero_buffer_u, 0, sizeof(s->just_a_zero_buffer_u));
    memset(s->just_a_zero_buffer_v, 0, sizeof(s->just_a_zero_buffer_v));
    memset(s->just_a_zero_buffer_w, 0, sizeof(s->just_a_zero_buffer_w));

    // In case of error:
    //return AVERROR_UNKNOWN;

    avctx->pix_fmt = AV_PIX_FMT_YUV420P;

    return 0;
}

static int fakeh264_decode_frame(AVCodecContext *avctx, void *data,
                            int *got_frame, AVPacket *avpkt)
{
    FakeH264DecoderContext *s = avctx->priv_data;
    int ret, linesize[3];
    AVFrame *avframe = data;
    uint8_t* ptrs[3];
    if (!avpkt->data) {
        // Here it means we reached end of stream and FFmpeg asks us for frames still in queue
        return 0;
    }

    if (avpkt->size < 6) {
        av_log(avctx, AV_LOG_INFO, "Got packet of length %d.\n", (int) avpkt->size);
    } else {
        av_log(avctx, AV_LOG_INFO, "Got packet of length %d beginning with bytes %02X%02X%02X%02X %02X%02X\n",
               (int) avpkt->size,
               (int) avpkt->data[0],
               (int) avpkt->data[1],
               (int) avpkt->data[2],
               (int) avpkt->data[3],
               (int) avpkt->data[4],
               (int) avpkt->data[5]);

    }
    
    //if (decoder has accepted our input data, but no output frame is available so far) {
    //    return avpkt->size;
    //}

    ret = ff_set_dimensions(avctx, 640, 480);
    if (ret < 0)
        return ret;

    if (ff_get_buffer(avctx, avframe, 0) < 0) {
        av_log(avctx, AV_LOG_ERROR, "Unable to allocate buffer\n");
        return AVERROR(ENOMEM);
    }

    linesize[0] = 640; // May be more than width, ask real codec instead for "stride" instead
    linesize[1] = linesize[2] = 320; // Typically half width, unless there is padding

    ptrs[0] = s->just_a_zero_buffer_u;
    ptrs[1] = s->just_a_zero_buffer_v;
    ptrs[2] = s->just_a_zero_buffer_w;

    // making it zero-copy is for further optimisations
    av_image_copy(avframe->data, avframe->linesize, (const uint8_t **) ptrs, linesize, avctx->pix_fmt, avctx->width, avctx->height);

    avframe->pts     = avpkt->pts; // can be different if we are returning some earlier frame
    avframe->pkt_dts = AV_NOPTS_VALUE;

    // I don't know why this thing is needed:
#if FF_API_PKT_PTS
FF_DISABLE_DEPRECATION_WARNINGS
    avframe->pkt_pts = avpkt->pts;
FF_ENABLE_DEPRECATION_WARNINGS
#endif

    *got_frame = 1;
    return avpkt->size;
}

AVCodec ff_fakeh264_decoder = {
    .name           = "fakeh264dec",
    .long_name      = NULL_IF_CONFIG_SMALL("A decoder stub that poses as H.264 decoder, but in reality just gives green frames"),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_H264,
    .priv_data_size = sizeof(FakeH264DecoderContext),
    .init           = fakeh264_decode_init,
    .decode         = fakeh264_decode_frame,
    .close          = fakeh264_decode_close,
    .capabilities   = 0
                    | AV_CODEC_CAP_DELAY   // We may delay frames and accept NULL input as a signal to deliver remaining frames
                    | AV_CODEC_CAP_DR1    // We are using ff_get_buffer
                    ,
    .caps_internal  = 0
                      | FF_CODEC_CAP_INIT_THREADSAFE // We are not using any global variables/resources, so mutex not needed
                      | FF_CODEC_CAP_INIT_CLEANUP // Call fakeh264_decode_close even if fakeh264_decode_init failed
                      ,
    .bsfs           = "h264_mp4toannexb", // It ensures encoded frames would begin with 00 00 00 01, like in raw h264 stream, not like in mp4 or mkv.
    .wrapper_name   = "fakeh264",
};
