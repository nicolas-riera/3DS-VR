#include "sysinfo.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "vr3ds_protocol.h"

#define IDENTITY_PATH APP_DIR "/identity.txt"

ConsoleInfo g_console;
static bool s_mcu;

static const char* const s_modelNames[VR3DS_MODEL_COUNT] = {
	"Nintendo 3DS", "Nintendo 3DS XL", "New Nintendo 3DS", "Nintendo 2DS", "New Nintendo 3DS XL",
	"New Nintendo 2DS XL",
};

const char* sysinfo_model_name(u8 model) {
	return model < VR3DS_MODEL_COUNT ? s_modelNames[model] : "Nintendo 3DS";
}

static u64 s_seed;

static u64 random64(void) {
	/* splitmix64 over the tick counter and the RTC: good enough for an identifier */
	s_seed += 0x9E3779B97F4A7C15ULL ^ svcGetSystemTick() ^ (osGetTime() << 20);
	u64 z = s_seed;
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
	return z ^ (z >> 31);
}

static void save_identity(void) {
	mkdir("sdmc:/3ds", 0777);
	mkdir(APP_DIR, 0777);
	FILE* f = fopen(IDENTITY_PATH, "w");
	if (!f) return;
	fprintf(f, "id=%016llx\ntoken=%016llx\n", g_console.consoleId, g_console.token);
	fclose(f);
}

static void load_identity(void) {
	g_console.consoleId = g_console.token = 0;
	FILE* f = fopen(IDENTITY_PATH, "r");
	if (f) {
		char line[64];
		while (fgets(line, sizeof(line), f)) {
			unsigned long long v;
			if (sscanf(line, "id=%llx", &v) == 1) g_console.consoleId = v;
			else if (sscanf(line, "token=%llx", &v) == 1) g_console.token = v;
		}
		fclose(f);
	}
	if (g_console.consoleId && g_console.token) return;
	if (!g_console.consoleId) g_console.consoleId = random64();
	if (!g_console.token) g_console.token = random64() | 1;
	save_identity();
}

void sysinfo_reset_pairing(void) {
	g_console.token = random64() | 1;
	save_identity();
}

static void load_name(void) {
	u16 utf16[0x1C / 2 + 1];
	memset(utf16, 0, sizeof(utf16));
	strcpy(g_console.name, "Nintendo 3DS");
	if (R_FAILED(CFGU_GetConfigInfoBlk2(0x1C, 0x000A0000, utf16))) return;
	utf16[0x1C / 2] = 0;
	u8 out[sizeof(g_console.name)];
	memset(out, 0, sizeof(out));
	ssize_t n = utf16_to_utf8(out, utf16, sizeof(out) - 1);
	if (n > 0) {
		out[n < (ssize_t)sizeof(out) ? n : (ssize_t)sizeof(out) - 1] = 0;
		strcpy(g_console.name, (const char*)out);
	}
}

void sysinfo_init(void) {
	memset(&g_console, 0, sizeof(g_console));
	s_seed = svcGetSystemTick();

	u8 model = CFG_MODEL_3DS;
	if (R_FAILED(CFGU_GetSystemModel(&model)) || model >= VR3DS_MODEL_COUNT) model = CFG_MODEL_3DS;
	g_console.model = model;

	bool isNew = false;
	APT_CheckNew3DS(&isNew);
	g_console.isNew = isNew;
	g_console.has3D = model != CFG_MODEL_2DS && model != CFG_MODEL_N2DSXL;

	load_name();
	load_identity();
	s_mcu = R_SUCCEEDED(mcuHwcInit());
}

void sysinfo_battery(u8* percent, u8* flags) {
	u8 pct = 0, charging = 0;
	bool adapter = false;
	if (!(s_mcu && R_SUCCEEDED(MCUHWC_GetBatteryLevel(&pct)))) {
		u8 level = 0; /* 0-5 */
		PTMU_GetBatteryLevel(&level);
		pct = level * 20;
	}
	if (pct > 100) pct = 100;
	PTMU_GetBatteryChargeState(&charging);
	PTMU_GetAdapterState(&adapter);
	*percent = pct;
	*flags = (charging ? VR3DS_BATT_CHARGING : 0) | (adapter ? VR3DS_BATT_ADAPTER : 0);
}
