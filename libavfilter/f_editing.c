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

#include <string.h>

#include "audio.h"
#include "video.h"
#include "libavutil/opt.h"


typedef struct _MediaSegment {
    double start;
    double end;
    struct _MediaSegment *next;
} MediaSegment;

typedef struct {
    const AVClass *class;
    char *opt_segments;
    double ts_base;
    double ts_prev;
    int frame_out;
    MediaSegment *current;
    MediaSegment *segments;
} EditingContext;


#define FLAGS ( \
    AV_OPT_FLAG_AUDIO_PARAM | \
    AV_OPT_FLAG_VIDEO_PARAM | \
    AV_OPT_FLAG_FILTERING_PARAM \
)
#define OFFSET(x) offsetof(EditingContext, x)
static const AVOption options[] = {
    { "segments", "set the segment list", OFFSET(opt_segments),
        AV_OPT_TYPE_STRING, { .str = NULL }, .flags=FLAGS },
    { NULL }
};


static int filter_frame(AVFilterLink *inlink, AVFrame *frame)
{
    int ret;
    double frame_in_ts, frame_out_ts;
    EditingContext *editing = inlink->dst->priv;

    if (!editing->current) /* fast-forward to the end */
        goto discard;

    frame_in_ts = (double) frame->pts * av_q2d(inlink->time_base);

    if (editing->ts_prev > frame_in_ts) {
        av_log(inlink->dst, AV_LOG_ERROR, "Frame discontinuity "
            "error %f\n", editing->ts_prev - frame_in_ts);
        av_frame_free(&frame);
        return AVERROR(EINVAL);
    }

    editing->ts_prev = frame_in_ts;
    frame_out_ts = editing->ts_base +
        (frame_in_ts - editing->current->start);

    if (frame_in_ts >= editing->current->end) {
        editing->current = editing->current->next;
        editing->ts_base = frame_out_ts;
        goto discard;
    }

    if (frame_in_ts <= editing->current->start) {
        goto discard;
    }

    frame->pts = frame_out_ts / av_q2d(inlink->time_base);

    ret = ff_filter_frame(inlink->dst->outputs[0], frame);
    editing->frame_out = (ret == 0) ? 1 : 0;

    return ret;

  discard:
    av_frame_free(&frame);
    editing->frame_out = 0;
    return 0;
}

static int request_frame(AVFilterLink *outlink)
{
    int ret;
    EditingContext *editing = outlink->src->priv;

    if (!editing->current) /* exit after last segment */
        return AVERROR_EOF;

    do {
        ret = ff_request_frame(outlink->src->inputs[0]);
        if (ret < 0)
            return ret;
    } while (!editing->frame_out);

    return 0;
}

static int parse_segments(AVFilterContext *ctx)
{
    char *n, *p;
    MediaSegment *segment, *j, **i;
    EditingContext *editing = ctx->priv;

    if (!editing->opt_segments) {
        av_log(ctx, AV_LOG_ERROR, "Missing segments list\n");
        return AVERROR(EINVAL);
    }

    j = NULL;
    i = &editing->segments;
    n = editing->opt_segments;

    do {
        if (!(p = strsep(&n, "-"))) {
            av_log(ctx, AV_LOG_ERROR, "No segments were specified\n");
            return AVERROR(EINVAL);
        }

        segment = av_malloc(sizeof(MediaSegment));
        segment->next = NULL;

        segment->start = atof(p);

        if (j && segment->start < j->end) {
            av_log(ctx, AV_LOG_ERROR, "Non-monotonic segments\n");
            return AVERROR(EINVAL);
        }

        if (!(p = strsep(&n, "#"))) {
            av_log(ctx, AV_LOG_ERROR, "Invalid segment list\n");
            return AVERROR(EINVAL);
        }

        segment->end = atof(p);

        if (segment->start >= segment->end) {
            av_log(ctx, AV_LOG_ERROR, "Invalid or empty segment\n");
            return AVERROR(EINVAL);
        }

        *i = j = segment, i = &segment->next;
    } while (n != NULL);

    return 0;
}

static av_cold int init(AVFilterContext *ctx)
{
    int ret;
    EditingContext *editing = ctx->priv;

    ret = parse_segments(ctx);
    if (ret < 0)
        return ret;

    editing->current = editing->segments;
    editing->ts_base = 0;
    editing->ts_prev = 0;
    editing->frame_out = 0;

    return 0;
}

static av_cold void uninit(AVFilterContext *ctx) {
    MediaSegment *n, *i;
    EditingContext *editing = ctx->priv;

    for (i = editing->segments; i != NULL; i = n) {
        n = i->next;
        av_freep(&i);
    }

    editing->current = editing->segments = NULL;
}

#define vediting_options options
AVFILTER_DEFINE_CLASS(vediting);

#define aediting_options options
AVFILTER_DEFINE_CLASS(aediting);

static const AVFilterPad avfilter_af_editing_inputs[] = {
    {
        .name           = "default",
        .type           = AVMEDIA_TYPE_AUDIO,
        .filter_frame   = filter_frame,
    },
    {NULL}
};

static const AVFilterPad avfilter_af_editing_outputs[] = {
    {
        .name           = "default",
        .type           = AVMEDIA_TYPE_AUDIO,
        .request_frame  = request_frame,
    },
    {NULL}
};

AVFilter ff_af_aediting = {
    .name           = "aediting",
    .description    = NULL_IF_CONFIG_SMALL("Select audio segments"),
    .init           = init,
    .uninit         = uninit,
    .priv_size      = sizeof(EditingContext),
    .priv_class     = &aediting_class,
    .inputs         = avfilter_af_editing_inputs,
    .outputs        = avfilter_af_editing_outputs,
};

static const AVFilterPad avfilter_vf_editing_inputs[] = {
    {
        .name           = "default",
        .type           = AVMEDIA_TYPE_VIDEO,
        .filter_frame   = filter_frame,
    },
    {NULL}
};

static const AVFilterPad avfilter_vf_editing_outputs[] = {
    {
        .name           = "default",
        .type           = AVMEDIA_TYPE_VIDEO,
        .request_frame  = request_frame,
    },
    {NULL}
};

AVFilter ff_vf_vediting = {
    .name           = "vediting",
    .description    = NULL_IF_CONFIG_SMALL("Select video segments"),
    .init           = init,
    .uninit         = uninit,
    .priv_size      = sizeof(EditingContext),
    .priv_class     = &vediting_class,
    .inputs         = avfilter_vf_editing_inputs,
    .outputs        = avfilter_vf_editing_outputs,
};
