/*
 *			GPAC - Multimedia Framework C SDK
 *
 *  This file is part of GPAC / AVS3 decoder filter, based on uavs3d.
 *
 *  AVS3 is the third-generation Chinese video standard (AVS3-P2). The test
 *  material for it comes as a raw elementary stream: start-code-delimited
 *  units, no container. Nothing in this tree demuxes that, so the filter takes
 *  the whole file, splits it on start codes and feeds uavs3d one unit at a
 *  time, emitting one packet per decoded frame - the same shape libgif and
 *  libmng use for animation.
 *
 *  Two constraints come from uavs3d itself:
 *
 *  - its frame callback is a bare function pointer with no user-data argument,
 *    so the filter context has to be reached through a file-static pointer.
 *    Only one AVS3 stream can therefore be decoded at a time in a module, and
 *    a second instance is refused rather than allowed to steal the first one's
 *    callback. Same situation as libtta.
 *  - it is built here with frm_threads = 1: side modules are single-threaded.
 *
 *  A note on the test signal, because the numbers look alarming at first:
 *  test_signals/AVS3/SFTI_640x360.bin contains 600 picture start codes but
 *  only 10 frames decode. That is not this filter - uavs3d's own uavs3dec,
 *  built natively from the same sources, decodes exactly the same 10 frames
 *  and then stops with "Failed: -6". The file is truncated or otherwise
 *  damaged past its tenth picture; parity with upstream is what the test
 *  checks. */

#include <gpac/filters.h>
#include <gpac/constants.h>
#include <string.h>
#include <stdlib.h>

#include <uavs3d.h>

/* AVS3 frame rate codes, as listed in the standard's sequence header. */
static const struct { u32 num, den; } avs3_frame_rates[] = {
	{0, 0}, {24000, 1001}, {24, 1}, {25, 1}, {30000, 1001}, {30, 1},
	{50, 1}, {60000, 1001}, {60, 1}, {100, 1}, {120, 1}, {200, 1},
	{240, 1}, {300, 1}, {0, 0}, {0, 0}
};

typedef struct
{
	GF_FilterPid *ipid, *opid;
	void *dec;
	Bool owns_decoder;
	u32 width, height, timescale, frame_dur;
	u64 next_cts;
	Bool cfg_done;
	GF_Err frame_err;
} GF_AVS3DecCtx;

/* uavs3d's callback carries no user pointer; see the header comment. */
static GF_AVS3DecCtx *avs3_active = NULL;

static void avs3dec_on_frame(uavs3d_io_frm_t *frm)
{
	GF_AVS3DecCtx *ctx = avs3_active;
	GF_FilterPacket *dst_pck;
	u8 *output;
	u32 w, h, i;

	if (!ctx || !frm || !frm->got_pic || !ctx->opid)
		return;
	if (frm->bit_depth != 8)
	{
		/* the library is built with COMPILE_10BIT=0, so this should not
		 * happen; refusing beats shipping mangled pixels */
		ctx->frame_err = GF_NOT_SUPPORTED;
		return;
	}

	w = (u32)frm->width[0];
	h = (u32)frm->height[0];
	if (!w || !h)
		return;

	if (!ctx->cfg_done || (w != ctx->width) || (h != ctx->height))
	{
		ctx->width = w;
		ctx->height = h;
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_WIDTH, &PROP_UINT(w));
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_HEIGHT, &PROP_UINT(h));
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_STRIDE, &PROP_UINT(w));
		if (frm->seqhdr)
		{
			u32 code = frm->seqhdr->frame_rate_code & 15;
			if (avs3_frame_rates[code].num)
			{
				ctx->timescale = avs3_frame_rates[code].num;
				ctx->frame_dur = avs3_frame_rates[code].den;
				gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_TIMESCALE, &PROP_UINT(ctx->timescale));
				gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_FPS,
				                           &PROP_FRAC_INT(ctx->timescale, ctx->frame_dur));
			}
		}
		ctx->cfg_done = GF_TRUE;
	}

	/* 4:2:0 planar, which is what GF_PIXEL_YUV means, but uavs3d hands over
	 * each plane with its own stride, so the copy is row by row. */
	dst_pck = gf_filter_pck_new_alloc(ctx->opid, w * h * 3 / 2, &output);
	if (!dst_pck)
	{
		ctx->frame_err = GF_OUT_OF_MEM;
		return;
	}
	for (i = 0; i < h; i++)
		memcpy(output + (size_t)i * w, (u8 *)frm->buffer[0] + (size_t)i * frm->stride[0], w);
	{
		u8 *dst_u = output + (size_t)w * h;
		u8 *dst_v = dst_u + (size_t)(w / 2) * (h / 2);
		for (i = 0; i < h / 2; i++)
		{
			memcpy(dst_u + (size_t)i * (w / 2), (u8 *)frm->buffer[1] + (size_t)i * frm->stride[1], w / 2);
			memcpy(dst_v + (size_t)i * (w / 2), (u8 *)frm->buffer[2] + (size_t)i * frm->stride[2], w / 2);
		}
	}

	gf_filter_pck_set_cts(dst_pck, ctx->next_cts);
	gf_filter_pck_set_duration(dst_pck, ctx->frame_dur);
	gf_filter_pck_set_sap(dst_pck, GF_FILTER_SAP_1);
	gf_filter_pck_send(dst_pck);
	ctx->next_cts += ctx->frame_dur;
}

static GF_Err avs3dec_configure_pid(GF_Filter *filter, GF_FilterPid *pid, Bool is_remove)
{
	GF_AVS3DecCtx *ctx = (GF_AVS3DecCtx *)gf_filter_get_udta(filter);

	if (is_remove)
	{
		if (ctx->opid)
		{
			gf_filter_pid_remove(ctx->opid);
			ctx->opid = NULL;
		}
		ctx->ipid = NULL;
		return GF_OK;
	}
	if (!gf_filter_pid_check_caps(pid))
		return GF_NOT_SUPPORTED;

	if (avs3_active && (avs3_active != ctx))
	{
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[AVS3Dec] uavs3d's frame callback carries no user data, so only one AVS3 stream can be decoded at a time\n"));
		return GF_NOT_SUPPORTED;
	}

	ctx->ipid = pid;
	gf_filter_pid_set_framing_mode(pid, GF_TRUE);

	if (!ctx->opid)
		ctx->opid = gf_filter_pid_new(filter);

	/* Corrected from the sequence header once a frame comes out; GPAC resolves
	 * the graph from these first. */
	ctx->width = 640;
	ctx->height = 360;
	ctx->timescale = 25;
	ctx->frame_dur = 1;
	ctx->next_cts = 0;
	ctx->cfg_done = GF_FALSE;
	ctx->frame_err = GF_OK;

	gf_filter_pid_copy_properties(ctx->opid, pid);
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_STREAM_TYPE, &PROP_UINT(GF_STREAM_VISUAL));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CODECID, &PROP_UINT(GF_CODECID_RAW));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_PIXFMT, &PROP_UINT(GF_PIXEL_YUV));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_WIDTH, &PROP_UINT(ctx->width));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_HEIGHT, &PROP_UINT(ctx->height));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_STRIDE, &PROP_UINT(ctx->width));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_TIMESCALE, &PROP_UINT(ctx->timescale));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_FPS, &PROP_FRAC_INT(ctx->timescale, ctx->frame_dur));

	return GF_OK;
}

/* Returns the offset of the next unit boundary at or after "from", or size
 * when there is none.
 *
 * AVS3 start codes are 00 00 01 xx, like MPEG's, but not every one of them
 * begins a new unit to hand the decoder: extensions, user data and the slices
 * that make up a picture all carry their own start code and belong *with* the
 * picture. uavs3d's own reader breaks only on the five codes below - slice 0,
 * sequence header, sequence end, I-picture and PB-picture - and that is what
 * is matched here. Splitting on every start code instead cuts pictures apart,
 * and the decoder then emits only a handful of frames out of the whole file. */
static u32 avs3_next_start_code(const u8 *data, u32 size, u32 from)
{
	u32 i;
	for (i = from; (i + 4) <= size; i++)
	{
		if (data[i] || data[i + 1] || (data[i + 2] != 1))
			continue;
		switch (data[i + 3])
		{
		case 0x00: /* slice 0, i.e. the start of a new picture's slice data */
		case 0xB0: /* sequence header */
		case 0xB1: /* sequence end */
		case 0xB3: /* I picture */
		case 0xB6: /* PB picture */
			return i;
		default:
			break;
		}
	}
	return size;
}

static GF_Err avs3dec_process(GF_Filter *filter)
{
	GF_FilterPacket *pck;
	u8 *data;
	u32 size, pos;
	int err = 0;
	uavs3d_cfg_t cfg;
	GF_AVS3DecCtx *ctx = (GF_AVS3DecCtx *)gf_filter_get_udta(filter);

	pck = gf_filter_pid_get_packet(ctx->ipid);
	if (!pck)
	{
		if (gf_filter_pid_is_eos(ctx->ipid))
		{
			gf_filter_pid_set_eos(ctx->opid);
			return GF_EOS;
		}
		return GF_OK;
	}
	data = (u8 *)gf_filter_pck_get_data(pck, &size);
	if (!data || (size < 4))
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_NON_COMPLIANT_BITSTREAM;
	}
	if (data[0] || data[1] || (data[2] != 1))
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[AVS3Dec] Stream does not start with an AVS3 start code\n"));
		return GF_NON_COMPLIANT_BITSTREAM;
	}

	memset(&cfg, 0, sizeof(cfg));
	cfg.frm_threads = 1; /* side modules are single-threaded */
	cfg.check_md5 = 0;
	ctx->dec = uavs3d_create(&cfg, avs3dec_on_frame, &err);
	if (!ctx->dec)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[AVS3Dec] Could not create the decoder (error %d)\n", err));
		return GF_NOT_SUPPORTED;
	}
	avs3_active = ctx;
	ctx->owns_decoder = GF_TRUE;

	/* The io struct is zeroed once and then only its bitstream fields are
	 * refreshed, which is what uavs3d's own reader does. Clearing it on every
	 * call instead loses the state the decoder keeps in it between units, and
	 * the symptom is precise: the first picture after each sequence header
	 * decodes and every following one is rejected. */
	{
		uavs3d_io_frm_t frm;
		memset(&frm, 0, sizeof(frm));

		pos = 0;
		while (pos < size)
		{
			u32 next = avs3_next_start_code(data, size, pos + 3);

			frm.bs = data + pos;
			frm.bs_len = (int)(next - pos);
			frm.pts = (long long)ctx->next_cts;
			uavs3d_decode(ctx->dec, &frm);
			if (ctx->frame_err != GF_OK)
				break;
			pos = next;
		}
	}

	/* frames the decoder was still holding back for reordering */
	if (ctx->frame_err == GF_OK)
	{
		uavs3d_io_frm_t frm;
		memset(&frm, 0, sizeof(frm));
		while (uavs3d_flush(ctx->dec, &frm) == 0)
		{
			if (ctx->frame_err != GF_OK)
				break;
			memset(&frm, 0, sizeof(frm));
		}
	}

	uavs3d_delete(ctx->dec);
	ctx->dec = NULL;
	avs3_active = NULL;
	ctx->owns_decoder = GF_FALSE;

	gf_filter_pid_drop_packet(ctx->ipid);
	if (ctx->frame_err != GF_OK)
		return ctx->frame_err;
	if (!ctx->cfg_done)
	{
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[AVS3Dec] No frame decoded\n"));
		return GF_NON_COMPLIANT_BITSTREAM;
	}
	gf_filter_pid_set_eos(ctx->opid);
	return GF_EOS;
}

static void avs3dec_finalize(GF_Filter *filter)
{
	GF_AVS3DecCtx *ctx = (GF_AVS3DecCtx *)gf_filter_get_udta(filter);
	if (ctx->dec)
	{
		uavs3d_delete(ctx->dec);
		ctx->dec = NULL;
	}
	if (avs3_active == ctx)
		avs3_active = NULL;
	ctx->owns_decoder = GF_FALSE;
}

static const GF_FilterCapability AVS3DecCaps[] =
	{
		CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_FILE),
		CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_FILE_EXT, "avs3|avs"),
		CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_MIME, "video/avs3|video/x-avs3"),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_VISUAL),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_CODECID, GF_CODECID_RAW),
};

GF_FilterRegister AVS3DecoderRegister = {
	.name = "avs3dec",
	GF_FS_SET_DESCRIPTION("AVS3 (AVS3-P2) video decoder")
		GF_FS_SET_HELP("This filter decodes raw AVS3 elementary streams using uavs3d, emitting one raw YUV 4:2:0 frame per picture.")
			.private_size = sizeof(GF_AVS3DecCtx),
	SETCAPS(AVS3DecCaps),
	.configure_pid = avs3dec_configure_pid,
	.process = avs3dec_process,
	.finalize = avs3dec_finalize,
};

const GF_FilterRegister *EMSCRIPTEN_KEEPALIVE avs3dec_register(GF_FilterSession *session)
{
	return &AVS3DecoderRegister;
}

#include "filter_register.h"
__attribute__((constructor))
void register_avs3dec(void) {
    gf_filter_auto_register("avs3dec", avs3dec_register);
}
