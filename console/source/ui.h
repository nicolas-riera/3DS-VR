#pragma once
#include <3ds.h>
#include <stdbool.h>

#include "input.h"
#include "net.h"
#include "video.h"

typedef struct {
	NetStatus net;
	VideoStats video;
	bool streaming;     /* a picture is on the top screen */
	bool calibrated;
	float calibProgress;
	bool sensorsOk;
	bool audioOk;
	u8 battery, batteryFlags;
} UiInfo;

typedef struct {
	bool recenter;
	bool settingsChanged;
	bool resetPairing;
	bool exitApp;
} UiActions;

bool ui_init(void);
void ui_exit(void);

/* Touch screen handling. Fills the touch part of the gamepad. */
void ui_update(u32 down, u32 held, TouchPad* pad, UiActions* act);
bool ui_in_settings(void);
/* Short message on the top screen. */
void ui_toast(const char* msg);

/* Top screen when there is no picture to show. */
void ui_draw_top_status(const UiInfo* info);
/* Over the video (toasts). */
void ui_draw_top_overlay(const UiInfo* info);
void ui_draw_bottom(const UiInfo* info);
