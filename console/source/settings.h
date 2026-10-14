#pragma once
#include <stdbool.h>

#include "vr3ds_protocol.h"

#define FOV_MIN 50
#define FOV_MAX 110
#define QUALITY_COUNT 4
#define HEIGHT_MIN 120
#define HEIGHT_MAX 220

typedef struct {
	int fovDeg;          /* horizontal field of view of the top screen */
	int quality;         /* 0..QUALITY_COUNT-1 */
	int fps;
	bool want3D;
	bool nintendoLayout; /* false: Xbox positions (B = Xbox A), true: matching letters */
	int heightCm;        /* the player's height, for the head height in SteamVR */
} Settings;

extern Settings g_settings;

/* Needs sysinfo_init() (defaults depend on the model). */
void settings_load(void);
void settings_save(void);

const char* settings_quality_name(int q);
int settings_bitrate_kbps(void);
/* Frame rates offered on this console, terminated by 0. */
const int* settings_fps_choices(void);

/* Whether the stream should currently carry both eyes (3D on and slider up). */
void settings_set_stereo_active(bool on);
bool settings_stereo_active(void);

void settings_stream(Vr3dsStreamSettings* out);
/* Visible half-FOV tangents of the top screen. */
void settings_screen_tangents(float* tanX, float* tanY);
