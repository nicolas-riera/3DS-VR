#include "ui.h"

#include <citro2d.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "render.h"
#include "settings.h"
#include "sysinfo.h"

/* ----------------------------------------------------------------- style */

#define C_BG        C2D_Color32(14, 16, 26, 255)
#define C_BG2       C2D_Color32(24, 28, 44, 255)
#define C_PANEL     C2D_Color32(34, 39, 60, 255)
#define C_PANEL_HI  C2D_Color32(58, 66, 100, 255)
#define C_ACCENT    C2D_Color32(92, 168, 255, 255)
#define C_ACCENT_HI C2D_Color32(150, 205, 255, 255)
#define C_TEXT      C2D_Color32(236, 239, 247, 255)
#define C_DIM       C2D_Color32(140, 150, 178, 255)
#define C_GREEN     C2D_Color32(96, 222, 146, 255)
#define C_ORANGE    C2D_Color32(255, 184, 82, 255)
#define C_RED       C2D_Color32(255, 98, 104, 255)

typedef enum {
	B_NONE,
	B_L3,
	B_R3,
	B_GUIDE,
	B_ZL,
	B_ZR,
	B_RECENTER,
	B_SETTINGS,
	B_EXIT,
	B_FOV_DOWN,
	B_FOV_UP,
	B_QUALITY_DOWN,
	B_QUALITY_UP,
	B_FPS_DOWN,
	B_FPS_UP,
	B_HEIGHT_DOWN,
	B_HEIGHT_UP,
	B_3D,
	B_LAYOUT,
	B_CONTROLS,
	B_BACK,
	B_RESET_PAIRING,
} ButtonId;

typedef struct {
	ButtonId id;
	float x, y, w, h;
	const char* label;
	bool momentary; /* active while held (gamepad buttons) */
} Button;

#define MAX_BUTTONS 16

/* settings rows */
#define ROW_Y(i) (27.0f + (i) * 25.0f)
#define ROW_H    22.0f
#define REPEAT_DELAY_MS 400
#define REPEAT_EVERY_MS 70

/* virtual right stick (Old 3DS family) */
#define STICK_X 238.0f
#define STICK_Y 158.0f
#define STICK_R 50.0f

static C2D_TextBuf s_tb;
static bool s_settings;
static Button s_buttons[MAX_BUTTONS];
static int s_buttonCount;
static ButtonId s_pressed = B_NONE;
static bool s_stickActive;
static float s_stickX, s_stickY;
static char s_toast[64];
static u64 s_toastUntil;
static u64 s_resetArmedUntil;
static u64 s_nextRepeat;

/* --------------------------------------------------------------- drawing */

static float text(float x, float y, float size, u32 color, u32 align, const char* fmt, ...) {
	char buf[160];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	C2D_Text t;
	C2D_TextParse(&t, s_tb, buf);
	C2D_TextOptimize(&t);
	C2D_DrawText(&t, C2D_WithColor | align, x, y, 0.5f, size, size, color);
	float w = 0, h = 0;
	C2D_TextGetDimensions(&t, size, size, &w, &h);
	return w;
}

static void rrect(float x, float y, float w, float h, float r, u32 c) {
	C2D_DrawRectSolid(x + r, y, 0.5f, w - 2 * r, h, c);
	C2D_DrawRectSolid(x, y + r, 0.5f, r, h - 2 * r, c);
	C2D_DrawRectSolid(x + w - r, y + r, 0.5f, r, h - 2 * r, c);
	C2D_DrawCircleSolid(x + r, y + r, 0.5f, r, c);
	C2D_DrawCircleSolid(x + w - r, y + r, 0.5f, r, c);
	C2D_DrawCircleSolid(x + r, y + h - r, 0.5f, r, c);
	C2D_DrawCircleSolid(x + w - r, y + h - r, 0.5f, r, c);
}

static void background(float w, float h) {
	C2D_DrawRectangle(0, 0, 0.5f, w, h, C_BG, C_BG, C_BG2, C_BG2);
}

static void battery_icon(float x, float y, u8 pct, u8 flags) {
	u32 c = pct <= 15 ? C_RED : (pct <= 35 ? C_ORANGE : C_GREEN);
	if (flags & VR3DS_BATT_CHARGING) c = C_ACCENT;
	C2D_DrawRectSolid(x, y, 0.5f, 22, 11, C_DIM);
	C2D_DrawRectSolid(x + 1, y + 1, 0.5f, 20, 9, C_BG);
	C2D_DrawRectSolid(x + 22, y + 3, 0.5f, 2, 5, C_DIM);
	C2D_DrawRectSolid(x + 2, y + 2, 0.5f, 18.0f * pct / 100.0f, 7, c);
	text(x - 4, y - 2, 0.42f, C_DIM, C2D_AlignRight, "%u%%", pct);
}

static void wifi_icon(float x, float y, int bars) {
	for (int i = 0; i < 3; i++) {
		float h = 4.0f + i * 3.5f;
		C2D_DrawRectSolid(x + i * 5, y + 11 - h, 0.5f, 3, h, i < bars ? C_TEXT : C2D_Color32(70, 76, 100, 255));
	}
}

static void header(const UiInfo* info, const char* title) {
	C2D_DrawRectSolid(0, 0, 0.5f, 320, 24, C2D_Color32(8, 10, 18, 255));
	text(8, 4, 0.55f, C_ACCENT, C2D_AlignLeft, "%s", title);
	battery_icon(290, 7, info->battery, info->batteryFlags);
	wifi_icon(232, 6, osGetWifiStrength());
}

/* --------------------------------------------------------------- buttons */

static void add_button(ButtonId id, float x, float y, float w, float h, const char* label, bool momentary) {
	if (s_buttonCount >= MAX_BUTTONS) return;
	s_buttons[s_buttonCount++] = (Button){id, x, y, w, h, label, momentary};
}

static void layout(bool streaming) {
	s_buttonCount = 0;
	if (s_settings) {
		add_button(B_FOV_DOWN, 168, ROW_Y(0), 36, ROW_H, "-", false);
		add_button(B_FOV_UP, 276, ROW_Y(0), 36, ROW_H, "+", false);
		add_button(B_HEIGHT_DOWN, 168, ROW_Y(1), 36, ROW_H, "-", false);
		add_button(B_HEIGHT_UP, 276, ROW_Y(1), 36, ROW_H, "+", false);
		add_button(B_QUALITY_DOWN, 168, ROW_Y(2), 36, ROW_H, "-", false);
		add_button(B_QUALITY_UP, 276, ROW_Y(2), 36, ROW_H, "+", false);
		add_button(B_FPS_DOWN, 168, ROW_Y(3), 36, ROW_H, "-", false);
		add_button(B_FPS_UP, 276, ROW_Y(3), 36, ROW_H, "+", false);
		if (g_console.has3D) add_button(B_3D, 168, ROW_Y(4), 144, ROW_H, NULL, false);
		add_button(B_LAYOUT, 168, ROW_Y(5), 144, ROW_H, NULL, false);
		add_button(B_CONTROLS, 168, ROW_Y(6), 144, ROW_H, NULL, false);
		add_button(B_BACK, 8, 204, 96, 30, "Back", false);
		add_button(B_RESET_PAIRING, 112, 204, 120, 30, NULL, false);
		add_button(B_EXIT, 240, 204, 72, 30, "Exit", false);
		return;
	}
	if (!streaming) {
		add_button(B_SETTINGS, 8, 196, 148, 36, "Settings", false);
		add_button(B_EXIT, 164, 196, 148, 36, "Exit", false);
		return;
	}
	if (g_console.isNew) {
		add_button(B_L3, 8, 32, 96, 72, "L3", true);
		add_button(B_GUIDE, 112, 32, 96, 72, "Guide", true);
		add_button(B_R3, 216, 32, 96, 72, "R3", true);
		add_button(B_RECENTER, 8, 112, 200, 60, "Recenter", false);
		add_button(B_SETTINGS, 216, 112, 96, 60, "Settings", false);
	} else {
		add_button(B_ZL, 8, 30, 92, 40, "ZL", true);
		add_button(B_ZR, 220, 30, 92, 40, "ZR", true);
		add_button(B_L3, 8, 78, 70, 42, "L3", true);
		add_button(B_R3, 84, 78, 70, 42, "R3", true);
		add_button(B_GUIDE, 8, 126, 146, 36, "Guide", true);
		add_button(B_RECENTER, 8, 168, 84, 36, "Recenter", false);
		add_button(B_SETTINGS, 98, 168, 56, 36, "Menu", false);
	}
}

static const Button* hit(float x, float y) {
	for (int i = 0; i < s_buttonCount; i++) {
		const Button* b = &s_buttons[i];
		if (x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h) return b;
	}
	return NULL;
}

static bool held_button(ButtonId id) {
	return s_pressed == id;
}

static const char* button_label(const Button* b) {
	static char buf[40];
	switch (b->id) {
	case B_3D:
		return g_settings.want3D ? "3D: On" : "3D: Off";
	case B_LAYOUT:
		return g_settings.nintendoLayout ? "Same letters" : "Xbox positions";
	case B_CONTROLS:
		return g_settings.controls == VR3DS_CONTROLS_VR ? "VR controllers" : "Xbox controller";
	case B_RESET_PAIRING:
		if (osGetTime() < s_resetArmedUntil) return "Tap again";
		return "Forget PCs";
	default:
		snprintf(buf, sizeof(buf), "%s", b->label ? b->label : "");
		return buf;
	}
}

static void draw_buttons(void) {
	for (int i = 0; i < s_buttonCount; i++) {
		const Button* b = &s_buttons[i];
		bool down = held_button(b->id);
		u32 bg = down ? C_ACCENT : C_PANEL;
		if (b->id == B_RECENTER && !down) bg = C2D_Color32(40, 70, 120, 255);
		if (b->id == B_RESET_PAIRING && osGetTime() < s_resetArmedUntil) bg = C2D_Color32(140, 50, 56, 255);
		rrect(b->x, b->y, b->w, b->h, 7, bg);
		float size = b->h >= 50 ? 0.7f : 0.55f;
		text(b->x + b->w / 2, b->y + b->h / 2 - size * 15.0f, size, down ? C_BG : C_TEXT, C2D_AlignCenter, "%s",
			 button_label(b));
	}
}

/* --------------------------------------------------------------- actions */

static void step_fps(int dir) {
	const int* c = settings_fps_choices();
	int n = 0, cur = 0;
	while (c[n]) {
		if (c[n] == g_settings.fps) cur = n;
		n++;
	}
	cur += dir;
	if (cur < 0) cur = 0;
	if (cur >= n) cur = n - 1;
	g_settings.fps = c[cur];
}

static void on_tap(ButtonId id, UiActions* act) {
	switch (id) {
	case B_RECENTER:
		act->recenter = true;
		break;
	case B_SETTINGS:
		s_settings = true;
		break;
	case B_BACK:
		s_settings = false;
		break;
	case B_EXIT:
		act->exitApp = true;
		break;
	case B_FOV_DOWN:
	case B_FOV_UP:
		g_settings.fovDeg += id == B_FOV_UP ? 5 : -5;
		if (g_settings.fovDeg < FOV_MIN) g_settings.fovDeg = FOV_MIN;
		if (g_settings.fovDeg > FOV_MAX) g_settings.fovDeg = FOV_MAX;
		act->settingsChanged = true;
		break;
	case B_QUALITY_DOWN:
	case B_QUALITY_UP:
		g_settings.quality += id == B_QUALITY_UP ? 1 : -1;
		if (g_settings.quality < 0) g_settings.quality = 0;
		if (g_settings.quality >= QUALITY_COUNT) g_settings.quality = QUALITY_COUNT - 1;
		act->settingsChanged = true;
		break;
	case B_FPS_DOWN:
	case B_FPS_UP:
		step_fps(id == B_FPS_UP ? 1 : -1);
		act->settingsChanged = true;
		break;
	case B_HEIGHT_DOWN:
	case B_HEIGHT_UP:
		g_settings.heightCm += id == B_HEIGHT_UP ? 1 : -1;
		if (g_settings.heightCm < HEIGHT_MIN) g_settings.heightCm = HEIGHT_MIN;
		if (g_settings.heightCm > HEIGHT_MAX) g_settings.heightCm = HEIGHT_MAX;
		act->settingsChanged = true;
		break;
	case B_3D:
		g_settings.want3D = !g_settings.want3D;
		act->settingsChanged = true;
		break;
	case B_LAYOUT:
		g_settings.nintendoLayout = !g_settings.nintendoLayout;
		act->settingsChanged = true;
		break;
	case B_CONTROLS:
		g_settings.controls = g_settings.controls == VR3DS_CONTROLS_VR ? VR3DS_CONTROLS_XBOX : VR3DS_CONTROLS_VR;
		act->settingsChanged = true;
		break;
	case B_RESET_PAIRING:
		if (osGetTime() < s_resetArmedUntil) {
			s_resetArmedUntil = 0;
			act->resetPairing = true;
		} else {
			s_resetArmedUntil = osGetTime() + 3000;
		}
		break;
	default:
		break;
	}
}

void ui_update(u32 down, u32 held, TouchPad* pad, UiActions* act) {
	memset(pad, 0, sizeof(*pad));
	memset(act, 0, sizeof(*act));

	bool streaming = net_is_streaming();
	layout(streaming);
	bool stickZone = streaming && !s_settings && !g_console.isNew;

	touchPosition tp;
	hidTouchRead(&tp);

	if (down & KEY_TOUCH) {
		const Button* b = hit(tp.px, tp.py);
		s_stickActive = false;
		s_pressed = B_NONE;
		if (b) {
			s_pressed = b->id;
			s_nextRepeat = osGetTime() + REPEAT_DELAY_MS;
			if (!b->momentary) on_tap(b->id, act);
		} else if (stickZone) {
			float dx = tp.px - STICK_X, dy = tp.py - STICK_Y;
			if (dx * dx + dy * dy <= (STICK_R * 1.6f) * (STICK_R * 1.6f)) s_stickActive = true;
		}
	}

	if (held & KEY_TOUCH) {
		if (s_stickActive) {
			float dx = (tp.px - STICK_X) / STICK_R, dy = (tp.py - STICK_Y) / STICK_R;
			float len = sqrtf(dx * dx + dy * dy);
			if (len > 1.0f) {
				dx /= len;
				dy /= len;
			}
			s_stickX = dx;
			s_stickY = dy;
		} else if (s_pressed != B_NONE) {
			/* momentary buttons release when the stylus slides off them */
			const Button* b = hit(tp.px, tp.py);
			if (!b || b->id != s_pressed) s_pressed = B_NONE;
			/* holding - / + on the FOV or the height repeats */
			bool repeats = s_pressed == B_FOV_DOWN || s_pressed == B_FOV_UP || s_pressed == B_HEIGHT_DOWN ||
						   s_pressed == B_HEIGHT_UP;
			if (repeats && osGetTime() >= s_nextRepeat) {
				s_nextRepeat = osGetTime() + REPEAT_EVERY_MS;
				on_tap(s_pressed, act);
			}
		}
	} else {
		s_pressed = B_NONE;
		s_stickActive = false;
		s_stickX = s_stickY = 0;
	}

	if (s_stickActive) {
		pad->rx = s_stickX;
		pad->ry = -s_stickY; /* screen y grows downwards */
	}
	pad->l3 = held_button(B_L3);
	pad->r3 = held_button(B_R3);
	pad->guide = held_button(B_GUIDE);
	pad->zl = held_button(B_ZL);
	pad->zr = held_button(B_ZR);
}

bool ui_in_settings(void) {
	return s_settings;
}

void ui_toast(const char* msg) {
	snprintf(s_toast, sizeof(s_toast), "%s", msg);
	s_toastUntil = osGetTime() + 1500;
}

/* ------------------------------------------------------------ top screen */

static void top_status(const UiInfo* info) {
	background(400, 240);
	text(200, 22, 1.1f, C_ACCENT, C2D_AlignCenter, "3DS VR");
	text(200, 58, 0.5f, C_DIM, C2D_AlignCenter, "%s", sysinfo_model_name(g_console.model));

	const NetStatus* n = &info->net;
	u32 ip = n->localIp;
	switch (n->state) {
	case NET_OFFLINE:
		text(200, 100, 0.7f, C_ORANGE, C2D_AlignCenter, "Connecting to Wi-Fi...");
		text(200, 126, 0.48f, C_DIM, C2D_AlignCenter, "Wi-Fi must be on, with an access point saved");
		text(200, 146, 0.48f, C_DIM, C2D_AlignCenter, "in System Settings (same network as your PC).");
		break;
	case NET_SEARCHING:
		text(200, 96, 0.7f, C_TEXT, C2D_AlignCenter, "Looking for your PC...");
		text(200, 126, 0.48f, C_DIM, C2D_AlignCenter, "Start 3DS VR on your PC (same network).");
		text(200, 146, 0.48f, C_DIM, C2D_AlignCenter, "This console: %lu.%lu.%lu.%lu", ip & 0xFF, (ip >> 8) & 0xFF,
			 (ip >> 16) & 0xFF, ip >> 24);
		break;
	case NET_PAIRING:
		text(200, 84, 0.55f, C_DIM, C2D_AlignCenter, "Pairing code");
		text(200, 104, 1.6f, C_TEXT, C2D_AlignCenter, "%04u", n->pairCode);
		text(200, 160, 0.48f, C_DIM, C2D_AlignCenter, "Click \"Pair\" in 3DS VR on %s", n->pcName);
		break;
	case NET_REJECTED:
		text(200, 100, 0.7f, C_RED, C2D_AlignCenter, "Pairing refused");
		text(200, 132, 0.5f, C_DIM, C2D_AlignCenter, "%s did not accept this console.", n->pcName);
		break;
	case NET_STREAMING:
		text(200, 96, 0.7f, C_GREEN, C2D_AlignCenter, "Connected to %s", n->pcName);
		text(200, 128, 0.5f, C_DIM, C2D_AlignCenter, "Waiting for the first picture...");
		break;
	}

	if (!info->sensorsOk) {
		text(200, 196, 0.48f, C_RED, C2D_AlignCenter, "Motion sensors unavailable");
	} else if (!info->calibrated) {
		text(200, 190, 0.48f, C_ORANGE, C2D_AlignCenter, "Put the console down to calibrate the gyroscope");
		C2D_DrawRectSolid(120, 212, 0.5f, 160, 6, C_PANEL);
		C2D_DrawRectSolid(120, 212, 0.5f, 160 * info->calibProgress, 6, C_ORANGE);
	} else if (!info->audioOk) {
		text(200, 200, 0.42f, C_DIM, C2D_AlignCenter, "No audio: sdmc:/3ds/dspfirm.cdc is missing (run DSP1)");
	}
}

void ui_draw_top_status(const UiInfo* info) {
	C2D_TargetClear(render_target(GFX_LEFT), C_BG);
	C2D_SceneBegin(render_target(GFX_LEFT));
	top_status(info);
}

void ui_draw_top_overlay(const UiInfo* info) {
	(void)info;
	if (osGetTime() >= s_toastUntil) return;
	int eyes = render_3d() ? 2 : 1;
	for (int e = 0; e < eyes; e++) {
		C2D_SceneBegin(render_target(e ? GFX_RIGHT : GFX_LEFT));
		rrect(130, 12, 140, 26, 8, C2D_Color32(10, 12, 20, 200));
		text(200, 16, 0.5f, C_TEXT, C2D_AlignCenter, "%s", s_toast);
	}
}

/* --------------------------------------------------------- bottom screen */

static void bottom_settings(const UiInfo* info) {
	header(info, "Settings");
	const float lx = 10, ty = 3;
	int inches = (int)(g_settings.heightCm / 2.54f + 0.5f);
	text(lx, ROW_Y(0) + ty, 0.5f, C_TEXT, C2D_AlignLeft, "Field of view");
	text(240, ROW_Y(0) + ty, 0.5f, C_TEXT, C2D_AlignCenter, "%d°", g_settings.fovDeg);
	text(lx, ROW_Y(1) + ty, 0.5f, C_TEXT, C2D_AlignLeft, "Your height");
	text(240, ROW_Y(1) + ty - 1, 0.5f, C_TEXT, C2D_AlignCenter, "%d cm", g_settings.heightCm);
	text(lx + 84, ROW_Y(1) + ty + 2, 0.38f, C_DIM, C2D_AlignLeft, "%d'%d\"", inches / 12, inches % 12);
	text(lx, ROW_Y(2) + ty, 0.5f, C_TEXT, C2D_AlignLeft, "Video quality");
	text(240, ROW_Y(2) + ty, 0.5f, C_TEXT, C2D_AlignCenter, "%s", settings_quality_name(g_settings.quality));
	text(lx, ROW_Y(3) + ty, 0.5f, C_TEXT, C2D_AlignLeft, "Frame rate");
	text(240, ROW_Y(3) + ty, 0.5f, C_TEXT, C2D_AlignCenter, "%d fps", g_settings.fps);
	if (g_console.has3D) text(lx, ROW_Y(4) + ty, 0.5f, C_TEXT, C2D_AlignLeft, "Stereoscopic 3D");
	text(lx, ROW_Y(5) + ty, 0.5f, C_TEXT, C2D_AlignLeft, "Face buttons");
	text(lx, ROW_Y(6) + ty, 0.5f, C_TEXT, C2D_AlignLeft, "Buttons on the PC");
	text(lx + 92, ROW_Y(2) + ty + 2, 0.38f, C_DIM, C2D_AlignLeft, "%.1f Mbps", settings_bitrate_kbps() / 1000.0f);
	draw_buttons();
}

static void bottom_idle(const UiInfo* info) {
	header(info, "3DS VR");
	const NetStatus* n = &info->net;
	if (n->state == NET_PAIRING) {
		text(160, 44, 0.5f, C_DIM, C2D_AlignCenter, "Pairing code");
		text(160, 62, 1.4f, C_TEXT, C2D_AlignCenter, "%04u", n->pairCode);
		text(160, 118, 0.45f, C_DIM, C2D_AlignCenter, "Check that the same code is shown on");
		text(160, 134, 0.45f, C_DIM, C2D_AlignCenter, "%s, then click \"Pair\" there.", n->pcName);
	} else {
		text(160, 44, 0.5f, C_TEXT, C2D_AlignCenter, "Use your 3DS as a SteamVR headset");
		text(14, 78, 0.45f, C_DIM, C2D_AlignLeft, "1. Run \"3DS VR\" on your PC.");
		text(14, 98, 0.45f, C_DIM, C2D_AlignLeft, "2. Stay on the same Wi-Fi network.");
		text(14, 118, 0.45f, C_DIM, C2D_AlignLeft, "3. Pair once with the code, then just play.");
		text(14, 150, 0.42f, C_DIM, C2D_AlignLeft, "SELECT + START recenters the view while playing.");
	}
	draw_buttons();
}

static void bottom_stream(const UiInfo* info) {
	header(info, "3DS VR");
	if (!g_console.isNew) {
		/* virtual right stick */
		C2D_DrawCircleSolid(STICK_X, STICK_Y, 0.5f, STICK_R + 6, C_PANEL);
		C2D_DrawCircleSolid(STICK_X, STICK_Y, 0.5f, STICK_R - 2, C_BG2);
		float kx = STICK_X + s_stickX * STICK_R * 0.8f, ky = STICK_Y + s_stickY * STICK_R * 0.8f;
		C2D_DrawCircleSolid(kx, ky, 0.5f, 18, s_stickActive ? C_ACCENT : C_PANEL_HI);
	}
	draw_buttons();

	const VideoStats* v = &info->video;
	const Vr3dsConfig* c = &info->net.config;
	if (g_console.isNew) {
		text(10, 182, 0.42f, C_DIM, C2D_AlignLeft, "%s", info->net.pcName);
		text(10, 196, 0.42f, C_DIM, C2D_AlignLeft, "%.0f fps \xC2\xB7 %.1f Mbps \xC2\xB7 decode %.1f ms", v->fps,
			 info->net.rxKbps / 1000.0f, v->decodeMs);
		text(10, 210, 0.42f, C_DIM, C2D_AlignLeft, "%s \xC2\xB7 %ux%u per eye \xC2\xB7 %s", c->stereo ? "3D" : "2D",
			 c->eyeWidth, c->eyeHeight, c->codec == VR3DS_CODEC_H264 ? "H.264" : "JPEG");
	} else {
		text(8, 212, 0.42f, C_DIM, C2D_AlignLeft, "%.0f fps \xC2\xB7 %.1f Mbps", v->fps, info->net.rxKbps / 1000.0f);
	}
}

void ui_draw_bottom(const UiInfo* info) {
	C3D_RenderTarget* t = render_bottom_target();
	C2D_TargetClear(t, C_BG);
	C2D_SceneBegin(t);
	background(320, 240);
	if (s_settings) bottom_settings(info);
	else if (net_is_streaming()) bottom_stream(info);
	else bottom_idle(info);
	C2D_TextBufClear(s_tb);
}

bool ui_init(void) {
	s_tb = C2D_TextBufNew(4096);
	return s_tb != NULL;
}

void ui_exit(void) {
	if (s_tb) C2D_TextBufDelete(s_tb);
	s_tb = NULL;
}
