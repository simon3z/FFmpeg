/*
 * Copyright (c) 2013 Federico Simoncelli <federico.simoncelli@gmail.com>
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

/**
 * @file
 * filter for selecting video and audio segments
 */

#include <float.h>

#include "audio.h"
#include "video.h"
#include "libavutil/opt.h"
#include "libavutil/fifo.h"


typedef struct {
    const   AVClass *class;
    int64_t bufsize;
    size_t  maxbufsize;
    double  last_ts;
    int     last_nb_samples;
    double  tolerance;
    AVFifoBuffer *fifo;
} FixPtsContext;


#define FIXPTS_FLAGS ( \
    AV_OPT_FLAG_AUDIO_PARAM | \
    AV_OPT_FLAG_VIDEO_PARAM | \
    AV_OPT_FLAG_FILTERING_PARAM)

static const AVOption options[] = {
    { "bufsize", "frame buffer size ", offsetof(FixPtsContext, bufsize),
        AV_OPT_TYPE_INT64, {.i64 = 96}, 1, UINT32_MAX, FIXPTS_FLAGS },
    { "tolerance", "frame pts tolerance", offsetof(FixPtsContext, tolerance),
        AV_OPT_TYPE_DOUBLE, {.dbl = 0.0000001}, 0, FLT_MAX, FIXPTS_FLAGS },
    { NULL }
};


static int process_next_frame(AVFilterLink *inlink)
{
    int i, fifo_size, drain_size;
    double frame_ti, frame_gap, cached_gap, best_gap;
    AVFrame *frame, **cached;
    FixPtsContext *fixpts = inlink->dst->priv;
    AVFilterLink *outlink = inlink->dst->outputs[0];

    av_fifo_generic_read(fixpts->fifo, &frame, sizeof(frame), NULL);

    if (inlink->type == AVMEDIA_TYPE_VIDEO)
        frame_ti = 1.0 / av_q2d(inlink->frame_rate);
    else
        frame_ti = 1.0 / inlink->sample_rate * fixpts->last_nb_samples;

    frame_gap = fabs(fixpts->last_ts -
        frame->pts * av_q2d(inlink->time_base) + frame_ti);

    av_log(outlink->src, AV_LOG_INFO, "pts: %li, ts: %g\n", frame->pts,
        frame->pts * av_q2d(inlink->time_base));

    if (frame_gap < fixpts->tolerance)
        goto submit_and_exit;

    av_log(outlink->src, AV_LOG_INFO, "Unexpected frame gap: "
        "%f (interval is %f, tolerance %f)\n", frame_gap, frame_ti,
        fixpts->tolerance);

    fifo_size = av_fifo_size(fixpts->fifo);
    drain_size = 0;
    best_gap = frame_gap;

    for (i = 0; i < fifo_size; i += sizeof(frame)) {
        cached = (AVFrame **) av_fifo_peek2(fixpts->fifo, i);

        cached_gap = fabs(fixpts->last_ts -
            (*cached)->pts * av_q2d(inlink->time_base) + frame_ti);

        if (cached_gap < best_gap) {
            drain_size = i;
            best_gap = cached_gap;
        }
    }

    if (best_gap >= frame_gap) {
        av_log(outlink->src, AV_LOG_INFO, "No lower gap has "
            "been found, pushing the frame anyway\n");
        goto submit_and_exit;
    }

    av_log(outlink->src, AV_LOG_INFO, "A lower gap %f has "
        "been found, discarding %li frame(s)\n", best_gap,
        drain_size / sizeof(frame) + 1);

    for (i = 0; i < drain_size; i += sizeof(frame)) {
        cached = (AVFrame **) av_fifo_peek2(fixpts->fifo, i);
        av_frame_free(cached);
    }

    av_frame_free(&frame);
    av_fifo_drain(fixpts->fifo, drain_size);

    return 0;

 submit_and_exit:

    fixpts->last_ts = frame->pts * av_q2d(inlink->time_base);
    fixpts->last_nb_samples = frame->nb_samples;
    return ff_filter_frame(outlink, frame);
}

static int filter_frame(AVFilterLink *inlink, AVFrame *frame)
{
    int ret = 0;
    FixPtsContext *fixpts = inlink->dst->priv;
    AVFilterLink *outlink = inlink->dst->outputs[0];

    if (av_fifo_size(fixpts->fifo) < fixpts->maxbufsize) {
        av_fifo_generic_write(fixpts->fifo, &frame, sizeof(frame), NULL);
    }

    if (av_fifo_size(fixpts->fifo) == fixpts->maxbufsize) {
        process_next_frame(inlink);
    }

    if (av_fifo_size(fixpts->fifo) > fixpts->maxbufsize) {
        av_log(outlink->src, AV_LOG_ERROR, "Frame buffer is broken\n");
        return AVERROR(EINVAL);
    }

    return ret;
}

static int request_frame(AVFilterLink *outlink)
{
    int ret;
    FixPtsContext *fixpts = outlink->src->priv;

    ret = ff_request_frame(outlink->src->inputs[0]);
    if (ret != AVERROR_EOF || av_fifo_size(fixpts->fifo) == 0)
        return ret;

    av_log(outlink->src, AV_LOG_INFO, "Flushing %li buffered frames\n",
        av_fifo_size(fixpts->fifo) / sizeof(AVFrame *));

    while (av_fifo_size(fixpts->fifo) > 0) {
        process_next_frame(outlink->src->inputs[0]);
        if (ret != 0)
            return ret;
    }

    return 0;
}

static av_cold int init(AVFilterContext *ctx) {
    FixPtsContext *fixpts = ctx->priv;

    fixpts->last_ts = 0;
    fixpts->last_nb_samples = 0;
    fixpts->maxbufsize = sizeof(AVFrame*) * fixpts->bufsize;

    fixpts->fifo = av_fifo_alloc(fixpts->maxbufsize);
    if (fixpts->fifo == NULL)
        return AVERROR(ENOMEM);

    return 0;
}

static av_cold void uninit(AVFilterContext *ctx) {
    FixPtsContext *fixpts = ctx->priv;

    av_fifo_free(fixpts->fifo);
    fixpts->fifo = NULL;
}

#define vfixpts_options options
AVFILTER_DEFINE_CLASS(vfixpts);

#define afixpts_options options
AVFILTER_DEFINE_CLASS(afixpts);

static const AVFilterPad avfilter_af_fixpts_inputs[] = {
    {
        .name           = "default",
        .type           = AVMEDIA_TYPE_AUDIO,
        .filter_frame   = filter_frame,
    },
    {NULL}
};

static const AVFilterPad avfilter_af_fixpts_outputs[] = {
    {
        .name           = "default",
        .type           = AVMEDIA_TYPE_AUDIO,
        .request_frame  = request_frame,
    },
    {NULL}
};

AVFilter ff_af_afixpts = {
    .name           = "afixpts",
    .description    = NULL_IF_CONFIG_SMALL("Discard samples with faulty pts"),
    .init           = init,
    .uninit         = uninit,
    .priv_size      = sizeof(FixPtsContext),
    .priv_class     = &afixpts_class,
    .inputs         = avfilter_af_fixpts_inputs,
    .outputs        = avfilter_af_fixpts_outputs,
};

static const AVFilterPad avfilter_vf_fixpts_inputs[] = {
    {
        .name           = "default",
        .type           = AVMEDIA_TYPE_VIDEO,
        .filter_frame   = filter_frame,
    },
    {NULL}
};

static const AVFilterPad avfilter_vf_fixpts_outputs[] = {
    {
        .name           = "default",
        .type           = AVMEDIA_TYPE_VIDEO,
        .request_frame  = request_frame,
    },
    {NULL}
};

AVFilter ff_vf_vfixpts = {
    .name           = "vfixpts",
    .description    = NULL_IF_CONFIG_SMALL("Discard frames with faulty pts"),
    .init           = init,
    .uninit         = uninit,
    .priv_size      = sizeof(FixPtsContext),
    .priv_class     = &vfixpts_class,
    .inputs         = avfilter_vf_fixpts_inputs,
    .outputs        = avfilter_vf_fixpts_outputs,
};
