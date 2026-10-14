#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>
#include <malloc.h>
#include <string.h>

#include "audio.h"
#include "input.h"
#include "net.h"
#include "render.h"
#include "settings.h"
#include "sysinfo.h"
#include "tracking.h"
#include "ui.h"
#include "video.h"
#include "wifilock.h"

#define SOC_BUFFER_SIZE   0x100000
#define STEREO_SWITCH_FRAMES 30 /* the 3D slider must stay put for 0.5 s */

int main(void) {
	osSetSpeedupEnable(true);

	gfxInitDefault();
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
	C2D_Prepare();

	cfguInit();
	ptmuInit();
	acInit();
	sysinfo_init();
	settings_load();

	u32* socBuffer = (u32*)memalign(0x1000, SOC_BUFFER_SIZE);
	if (socBuffer && R_FAILED(socInit(socBuffer, SOC_BUFFER_SIZE))) {
		free(socBuffer);
		socBuffer = NULL;
	}
	/* no StreetPass / SpotPass traffic competing with the stream */
	wifilock_begin();

	bool ok = render_init() && ui_init() && video_init();
	audio_init();
	if (ok) ok = net_init();
	tracking_start();

	bool wasStreaming = false;
	bool comboLatched = false;
	bool stereoActive = settings_stereo_active();
	int stereoStable = 0;
	u64 settingsSaveAt = 0;

	while (ok && aptMainLoop()) {
		hidScanInput();
		u32 down = hidKeysDown();
		u32 held = hidKeysHeld();

		TouchPad touch;
		UiActions act;
		ui_update(down, held, &touch, &act);

		/* SELECT + START: recenter (both are kept away from the game meanwhile) */
		if ((held & KEY_SELECT) && (held & KEY_START)) {
			if (!comboLatched) act.recenter = true;
			comboLatched = true;
		} else if (!(held & (KEY_SELECT | KEY_START))) {
			comboLatched = false;
		}
		u32 masked = comboLatched ? (KEY_SELECT | KEY_START) : 0;

		if (act.exitApp) break;
		if (act.recenter) {
			tracking_recenter();
			ui_toast("View recentered");
		}
		if (act.resetPairing) {
			sysinfo_reset_pairing();
			net_disconnect();
			ui_toast("Pairing reset");
		}
		if (act.settingsChanged) {
			settingsSaveAt = osGetTime() + 1000; /* holding + / - changes them quickly */
			settings_set_stereo_active(stereoActive);
			net_settings_changed();
		}
		if (settingsSaveAt && osGetTime() >= settingsSaveAt) {
			settingsSaveAt = 0;
			settings_save();
		}

		/* Stereo only while 3D is enabled and the slider is up: a 2D stream
		   halves the bitrate needed for the same picture. */
		float slider = osGet3DSliderState();
		bool wantStereo = g_settings.want3D && g_console.has3D && slider > 0.0f;
		if (wantStereo != stereoActive) {
			if (++stereoStable >= STEREO_SWITCH_FRAMES) {
				stereoActive = wantStereo;
				stereoStable = 0;
				settings_set_stereo_active(stereoActive);
				net_settings_changed();
			}
		} else {
			stereoStable = 0;
		}

		Vr3dsGamepad pad;
		if (ui_in_settings()) memset(&pad, 0, sizeof(pad));
		else input_build(&pad, held, masked, &touch, g_settings.nintendoLayout);
		tracking_set_pad(&pad, (u8)(slider * 255.0f));

		UiInfo info;
		memset(&info, 0, sizeof(info));
		net_status(&info.net);
		bool streaming = info.net.state == NET_STREAMING && info.net.hasConfig;
		if (streaming && !wasStreaming) tracking_recenter();
		if (!streaming && wasStreaming) video_reset();
		wasStreaming = streaming;
		if (streaming) video_update(&info.net.config);

		VideoView view;
		bool hasView = streaming && video_view(&view);
		render_set_3d(hasView && g_console.has3D && g_settings.want3D && slider > 0.0f);

		video_stats(&info.video);
		info.streaming = hasView;
		info.sensorsOk = tracking_sensors_ok();
		info.calibrated = tracking_calibrated(&info.calibProgress);
		info.audioOk = audio_available();
		tracking_battery(&info.battery, &info.batteryFlags);

		float tanX, tanY;
		settings_screen_tangents(&tanX, &tanY);

		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		if (hasView) {
			render_video(&view, tracking_orientation(), tanX, tanY);
			C2D_Prepare();
			ui_draw_top_overlay(&info);
		} else {
			C2D_Prepare();
			ui_draw_top_status(&info);
		}
		ui_draw_bottom(&info);
		C3D_FrameEnd(0);
	}

	if (settingsSaveAt) settings_save();
	tracking_stop();
	net_exit();
	audio_exit();
	video_exit();
	ui_exit();
	render_exit();

	wifilock_end();
	if (socBuffer) {
		socExit();
		free(socBuffer);
	}
	acExit();
	ptmuExit();
	cfguExit();
	C2D_Fini();
	C3D_Fini();
	gfxExit();
	return 0;
}
