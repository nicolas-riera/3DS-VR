#include "video.h"

#include <stdio.h>
/* jpeglib.h needs FILE */
#include <jpeglib.h>
#include <setjmp.h>
#include <string.h>

#include "net.h"
#include "sysinfo.h"

#define TEX_TRANSFER_FLAGS                                                                          \
	(GX_TRANSFER_FLIP_VERT(1) | GX_TRANSFER_OUT_TILED(1) | GX_TRANSFER_RAW_COPY(0) |                \
	 GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGB565) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) | \
	 GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

#define MAX_DECODES_PER_UPDATE 4
#define TICKS_PER_US (SYSCLOCK_ARM11 / 1000000.0)

static C3D_Tex s_tex[2];
static bool s_texOk;
static int s_cur = -1;
static u8* s_out; /* linear, VIDEO_TEX_SIZE^2 RGB565: decoder output, DMA'd into the texture */
static u8* s_nal; /* linear, MVD input */

static bool s_mvdReady;
static MVDSTD_Config s_mvdCfg;
static u16 s_mvdW, s_mvdH;

static Vr3dsConfig s_cfg;
static bool s_haveCfg;
static bool s_needKey, s_haveLast;
static u32 s_lastId;
static bool s_hasImage;
static quat s_pose;

static LightLock s_statsLock;
static VideoStats s_stats;
static u64 s_shownRecvTick;
static u32 s_dropped;
static u32 s_fpsCount;
static u64 s_fpsWindow;
static double s_decTicks;
static u32 s_decCount;

/* --------------------------------------------------------------- H.264 */

static bool mvd_start(void) {
	if (s_mvdReady) return true;
	if (R_FAILED(mvdstdInit(MVDMODE_VIDEOPROCESSING, MVD_INPUT_H264, MVD_OUTPUT_BGR565, MVD_DEFAULT_WORKBUF_SIZE, NULL)))
		return false;
	s_mvdReady = true;
	return true;
}

static bool mvd_setup(u16 w, u16 h) {
	if (!g_console.hwH264) return false;
	/* a new picture size needs a fresh decoder */
	if (s_mvdReady && s_mvdW && (w != s_mvdW || h != s_mvdH)) {
		mvdstdExit();
		s_mvdReady = false;
	}
	if (!mvd_start()) return false;
	s_mvdW = w;
	s_mvdH = h;
	/* MVD's "BGR565" is the GPU's RGB565 */
	mvdstdGenerateDefaultConfig(&s_mvdCfg, w, h, w, h, NULL, (u32*)s_out, (u32*)s_out);
	s_mvdCfg.flag_x104 = 1;
	s_mvdCfg.output_x_pos = 0;
	s_mvdCfg.output_y_pos = 0;
	s_mvdCfg.output_width_override = VIDEO_TEX_SIZE; /* row pitch of the output buffer */
	s_mvdCfg.output_height_override = VIDEO_TEX_SIZE;
	return true;
}

/* Feeds an Annex-B access unit one NAL at a time (each with a 3-byte start
   code, as MVD expects). Returns true once a picture was rendered. */
static bool mvd_decode(const u8* au, u32 len) {
	if (!s_mvdReady) return false;
	bool rendered = false;
	u32 i = 0;
	/* find the first start code */
	while (i + 3 <= len && !(au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1)) i++;
	while (i + 3 <= len) {
		u32 start = i + 3;
		u32 j = start;
		while (j + 3 <= len && !(au[j] == 0 && au[j + 1] == 0 && au[j + 2] == 1)) j++;
		u32 end = (j + 3 <= len) ? j : len;
		u32 next = end;
		while (end > start && au[end - 1] == 0) end--; /* zero byte of a 4-byte start code */
		i = next;

		u32 n = end - start;
		if (n == 0 || n + 3 > VR3DS_MAX_FRAME) continue;
		int type = au[start] & 0x1F;
		/* slices, IDR slices, SPS and PPS only: SEI / AUD / filler are not needed */
		if (type != 1 && type != 5 && type != 7 && type != 8) continue;

		s_nal[0] = 0;
		s_nal[1] = 0;
		s_nal[2] = 1;
		memcpy(s_nal + 3, au + start, n);
		GSPGPU_FlushDataCache(s_nal, n + 3);
		Result ret = mvdstdProcessVideoFrame(s_nal, n + 3, 0, NULL);
		if (!MVD_CHECKNALUPROC_SUCCESS(ret)) return false;
		if ((type == 1 || type == 5) && ret != MVD_STATUS_PARAMSET && ret != MVD_STATUS_INCOMPLETEPROCESSING) {
			if (mvdstdRenderVideoFrame(&s_mvdCfg, true) == MVD_STATUS_OK) rendered = true;
		}
	}
	return rendered;
}

/* ---------------------------------------------------------------- JPEG */

typedef struct {
	struct jpeg_error_mgr pub;
	jmp_buf jump;
} JpegErr;

static void jpeg_error_exit(j_common_ptr cinfo) {
	JpegErr* e = (JpegErr*)cinfo->err;
	longjmp(e->jump, 1);
}

static void jpeg_silent(j_common_ptr cinfo, int level) {
	(void)cinfo;
	(void)level;
}

static bool jpeg_decode(const u8* data, u32 len) {
	struct jpeg_decompress_struct cinfo;
	JpegErr err;
	cinfo.err = jpeg_std_error(&err.pub);
	err.pub.error_exit = jpeg_error_exit;
	err.pub.emit_message = jpeg_silent;
	if (setjmp(err.jump)) {
		jpeg_destroy_decompress(&cinfo);
		return false;
	}
	jpeg_create_decompress(&cinfo);
	jpeg_mem_src(&cinfo, (unsigned char*)data, len);
	if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
		jpeg_destroy_decompress(&cinfo);
		return false;
	}
	cinfo.out_color_space = JCS_RGB565;
	cinfo.dct_method = JDCT_IFAST;
	cinfo.do_fancy_upsampling = FALSE;
	cinfo.do_block_smoothing = FALSE;
	cinfo.dither_mode = JDITHER_NONE;
	jpeg_start_decompress(&cinfo);
	if (cinfo.output_width > VIDEO_TEX_SIZE || cinfo.output_height > VIDEO_TEX_SIZE) {
		jpeg_abort_decompress(&cinfo);
		jpeg_destroy_decompress(&cinfo);
		return false;
	}
	while (cinfo.output_scanline < cinfo.output_height) {
		JSAMPROW row = s_out + cinfo.output_scanline * VIDEO_TEX_SIZE * 2;
		jpeg_read_scanlines(&cinfo, &row, 1);
	}
	u32 rows = cinfo.output_height;
	jpeg_finish_decompress(&cinfo);
	jpeg_destroy_decompress(&cinfo);
	GSPGPU_FlushDataCache(s_out, rows * VIDEO_TEX_SIZE * 2);
	return true;
}

/* ---------------------------------------------------------------- core */

bool video_init(void) {
	LightLock_Init(&s_statsLock);
	s_out = (u8*)linearMemAlign(VIDEO_TEX_SIZE * VIDEO_TEX_SIZE * 2, 0x80);
	s_nal = (u8*)linearMemAlign(VR3DS_MAX_FRAME + 16, 0x80);
	if (!s_out || !s_nal) return false;
	memset(s_out, 0, VIDEO_TEX_SIZE * VIDEO_TEX_SIZE * 2);
	/* MVD writes this buffer by DMA: no dirty cache line may be written back over it later */
	GSPGPU_FlushDataCache(s_out, VIDEO_TEX_SIZE * VIDEO_TEX_SIZE * 2);
	for (int i = 0; i < 2; i++) {
		if (!C3D_TexInit(&s_tex[i], VIDEO_TEX_SIZE, VIDEO_TEX_SIZE, GPU_RGB565)) return false;
		C3D_TexSetFilter(&s_tex[i], GPU_LINEAR, GPU_LINEAR);
		C3D_TexSetWrap(&s_tex[i], GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
	}
	s_texOk = true;
	s_fpsWindow = osGetTime();
	/* only advertise hardware H.264 if the decoder really starts (it needs ~9 MB
	   of linear memory); otherwise the PC sends JPEG */
	g_console.hwH264 = g_console.isNew && mvd_start();
	return true;
}

void video_exit(void) {
	/* mvdstdExit() can hang for a minute on some New 2DS XL units; the system
	   cleans up when the process exits anyway */
	if (s_mvdReady && g_console.model != CFG_MODEL_N2DSXL) mvdstdExit();
	s_mvdReady = false;
	if (s_texOk) {
		C3D_TexDelete(&s_tex[0]);
		C3D_TexDelete(&s_tex[1]);
		s_texOk = false;
	}
	if (s_out) linearFree(s_out);
	if (s_nal) linearFree(s_nal);
	s_out = s_nal = NULL;
}

void video_reset(void) {
	s_haveCfg = false;
	s_hasImage = false;
	s_haveLast = false;
}

static void reconfigure(const Vr3dsConfig* cfg) {
	s_cfg = *cfg;
	s_haveCfg = true;
	s_hasImage = false;
	s_haveLast = false;
	s_needKey = cfg->codec == VR3DS_CODEC_H264;
	if (cfg->codec == VR3DS_CODEC_H264) {
		mvd_setup(cfg->eyeWidth, cfg->eyeHeight * (cfg->stereo ? 2 : 1));
		/* MVD shows grey until it gets an IDR frame */
		net_request_keyframe(cfg->configId);
	}
}

void video_update(const Vr3dsConfig* cfg) {
	if (!s_texOk) return;
	if (!s_haveCfg || cfg->configId != s_cfg.configId || cfg->sessionId != s_cfg.sessionId) reconfigure(cfg);

	bool h264 = s_cfg.codec == VR3DS_CODEC_H264;
	bool decoded = false;
	NetFrame shown = {0};

	for (int k = 0; k < MAX_DECODES_PER_UPDATE; k++) {
		NetFrame f;
		if (!net_pop_frame(&f, !h264)) break;
		if (f.configId != s_cfg.configId) {
			net_release_frame(&f);
			continue;
		}
		u64 t0 = svcGetSystemTick();
		bool ok = false;
		if (h264) {
			bool key = (f.flags & VR3DS_FRAME_KEY) != 0;
			if (!key && s_haveLast && (s32)(f.frameId - s_lastId) <= 0) {
				/* an older frame that completed late: already past it */
				net_release_frame(&f);
				continue;
			}
			bool gap = s_haveLast && f.frameId != s_lastId + 1;
			if (!key && (s_needKey || gap)) {
				/* references are missing: wait for the next key frame */
				s_needKey = true;
				s_dropped++;
				net_request_keyframe(s_cfg.configId);
			} else {
				ok = mvd_decode(f.data, f.size);
				if (ok) {
					s_haveLast = true;
					s_lastId = f.frameId;
					s_needKey = false;
				} else {
					s_needKey = true;
					s_dropped++;
					net_request_keyframe(s_cfg.configId);
				}
			}
		} else {
			ok = jpeg_decode(f.data, f.size);
			if (!ok) s_dropped++;
		}
		if (ok) {
			s_decTicks += (double)(svcGetSystemTick() - t0);
			s_decCount++;
			decoded = true;
			shown = f;
		}
		net_release_frame(&f);
	}

	if (decoded) {
		int back = s_cur < 0 ? 0 : 1 - s_cur;
		C3D_SyncDisplayTransfer((u32*)s_out, GX_BUFFER_DIM(VIDEO_TEX_SIZE, VIDEO_TEX_SIZE), (u32*)s_tex[back].data,
								GX_BUFFER_DIM(VIDEO_TEX_SIZE, VIDEO_TEX_SIZE), TEX_TRANSFER_FLAGS);
		s_cur = back;
		s_hasImage = true;
		s_pose = shown.pose;
		s_fpsCount++;

		LightLock_Lock(&s_statsLock);
		s_stats.shownFrameId = shown.frameId;
		s_stats.shownFrameTs = shown.timestampUs;
		s_shownRecvTick = shown.recvTick;
		s_stats.framesDecoded++;
		LightLock_Unlock(&s_statsLock);
	}

	u64 now = osGetTime();
	if (now - s_fpsWindow >= 1000) {
		LightLock_Lock(&s_statsLock);
		s_stats.fps = (float)s_fpsCount * 1000.0f / (float)(now - s_fpsWindow);
		s_stats.decodeMs = s_decCount ? (float)(s_decTicks / s_decCount / TICKS_PER_US / 1000.0) : 0.0f;
		LightLock_Unlock(&s_statsLock);
		s_fpsCount = 0;
		s_decTicks = 0;
		s_decCount = 0;
		s_fpsWindow = now;
	}
}

bool video_view(VideoView* out) {
	if (!s_haveCfg || !s_hasImage || s_cur < 0) return false;
	float ew = s_cfg.eyeWidth, eh = s_cfg.eyeHeight, ts = VIDEO_TEX_SIZE;
	out->tex = &s_tex[s_cur];
	out->pose = s_pose;
	out->stereo = s_cfg.stereo != 0;
	out->encTanX = s_cfg.encTanX;
	out->encTanY = s_cfg.encTanY;
	out->u1 = ew / ts;
	/* after the vertical flip, row 0 of the decoded picture is at v = 1; the
	   half-texel inset keeps linear filtering from mixing the two eyes */
	float half = 0.5f / ts;
	out->vLeft[0] = 1.0f - eh / ts + half;
	out->vLeft[1] = 1.0f;
	if (out->stereo) {
		out->vRight[0] = 1.0f - 2.0f * eh / ts;
		out->vRight[1] = 1.0f - eh / ts - half;
	} else {
		out->vRight[0] = out->vLeft[0];
		out->vRight[1] = out->vLeft[1];
	}
	return true;
}

void video_stats(VideoStats* out) {
	LightLock_Lock(&s_statsLock);
	*out = s_stats;
	u64 recv = s_shownRecvTick;
	LightLock_Unlock(&s_statsLock);
	out->framesDropped = s_dropped + net_frames_lost();
	out->shownFrameAgeUs = recv ? (u32)((double)(svcGetSystemTick() - recv) / TICKS_PER_US) : 0;
}
