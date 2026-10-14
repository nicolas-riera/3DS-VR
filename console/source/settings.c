#include "settings.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "sysinfo.h"

#define SETTINGS_PATH APP_DIR "/settings.ini"

Settings g_settings;
static bool s_stereoActive;

static const char* const s_qualityNames[QUALITY_COUNT] = {"Low", "Medium", "High", "Ultra"};
/* H.264 on New 3DS / JPEG on Old 3DS (JPEG needs more bits for the same picture) */
static const int s_bitrateNew[QUALITY_COUNT] = {2000, 3500, 5000, 7000};
static const int s_bitrateOld[QUALITY_COUNT] = {1500, 2500, 3500, 5000};
static const int s_fpsNew[] = {30, 60, 0};
static const int s_fpsOld[] = {20, 30, 0};

const char* settings_quality_name(int q) {
	return (q >= 0 && q < QUALITY_COUNT) ? s_qualityNames[q] : "?";
}

int settings_bitrate_kbps(void) {
	int q = g_settings.quality;
	if (q < 0) q = 0;
	if (q >= QUALITY_COUNT) q = QUALITY_COUNT - 1;
	return g_console.isNew ? s_bitrateNew[q] : s_bitrateOld[q];
}

const int* settings_fps_choices(void) {
	return g_console.isNew ? s_fpsNew : s_fpsOld;
}

static void defaults(void) {
	g_settings.fovDeg = 80;
	g_settings.quality = 1;
	g_settings.fps = g_console.isNew ? 60 : 30;
	/* software decoding on Old 3DS: one view by default */
	g_settings.want3D = g_console.has3D && g_console.isNew;
	g_settings.nintendoLayout = false;
	g_settings.heightCm = 170;
}

static void sanitize(void) {
	if (g_settings.fovDeg < FOV_MIN) g_settings.fovDeg = FOV_MIN;
	if (g_settings.fovDeg > FOV_MAX) g_settings.fovDeg = FOV_MAX;
	if (g_settings.quality < 0 || g_settings.quality >= QUALITY_COUNT) g_settings.quality = 1;
	bool fpsOk = false;
	for (const int* f = settings_fps_choices(); *f; f++) fpsOk |= *f == g_settings.fps;
	if (!fpsOk) g_settings.fps = g_console.isNew ? 60 : 30;
	if (!g_console.has3D) g_settings.want3D = false;
	if (g_settings.heightCm < HEIGHT_MIN) g_settings.heightCm = HEIGHT_MIN;
	if (g_settings.heightCm > HEIGHT_MAX) g_settings.heightCm = HEIGHT_MAX;
}

void settings_load(void) {
	defaults();
	FILE* f = fopen(SETTINGS_PATH, "r");
	if (f) {
		char line[96];
		int v;
		while (fgets(line, sizeof(line), f)) {
			if (sscanf(line, "fov=%d", &v) == 1) g_settings.fovDeg = v;
			else if (sscanf(line, "quality=%d", &v) == 1) g_settings.quality = v;
			else if (sscanf(line, "fps=%d", &v) == 1) g_settings.fps = v;
			else if (sscanf(line, "3d=%d", &v) == 1) g_settings.want3D = v != 0;
			else if (sscanf(line, "nintendo_layout=%d", &v) == 1) g_settings.nintendoLayout = v != 0;
			else if (sscanf(line, "height_cm=%d", &v) == 1) g_settings.heightCm = v;
		}
		fclose(f);
	}
	sanitize();
	s_stereoActive = g_settings.want3D;
}

void settings_save(void) {
	sanitize();
	mkdir("sdmc:/3ds", 0777);
	mkdir(APP_DIR, 0777);
	FILE* f = fopen(SETTINGS_PATH, "w");
	if (!f) return;
	fprintf(f, "fov=%d\nquality=%d\nfps=%d\n3d=%d\nnintendo_layout=%d\nheight_cm=%d\n", g_settings.fovDeg,
			g_settings.quality, g_settings.fps, g_settings.want3D ? 1 : 0, g_settings.nintendoLayout ? 1 : 0,
			g_settings.heightCm);
	fclose(f);
}

void settings_set_stereo_active(bool on) {
	s_stereoActive = on && g_settings.want3D && g_console.has3D;
}

bool settings_stereo_active(void) {
	return s_stereoActive;
}

void settings_screen_tangents(float* tanX, float* tanY) {
	float tx = tanf((float)g_settings.fovDeg * 0.5f * (float)M_PI / 180.0f);
	*tanX = tx;
	*tanY = tx * 240.0f / 400.0f;
}

void settings_stream(Vr3dsStreamSettings* out) {
	memset(out, 0, sizeof(*out));
	out->stereo = s_stereoActive ? 1 : 0;
	out->fps = (uint8_t)g_settings.fps;
	out->bitrateKbps = (uint16_t)settings_bitrate_kbps();
	settings_screen_tangents(&out->tanX, &out->tanY);
	out->heightCm = (uint16_t)g_settings.heightCm;
}
