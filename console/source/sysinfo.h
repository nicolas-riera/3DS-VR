#pragma once
#include <3ds.h>
#include <stdbool.h>

#define APP_DIR "sdmc:/3ds/3DS-VR"

typedef struct {
	u8 model;        /* Vr3dsModel / CFG_SystemModel */
	bool isNew;      /* New 3DS family: hardware H.264 decoder, C-Stick, ZL/ZR */
	bool has3D;      /* not a 2DS / New 2DS XL */
	u64 consoleId;   /* random, generated on first launch */
	u64 token;       /* pairing secret, generated on first launch */
	char name[32];   /* console owner nickname */
} ConsoleInfo;

extern ConsoleInfo g_console;

/* Needs cfgu and the SD card. */
void sysinfo_init(void);
/* New token: every PC will have to pair again. */
void sysinfo_reset_pairing(void);

const char* sysinfo_model_name(u8 model);

/* Battery level in percent and VR3DS_BATT_* flags. Needs ptmu (and mcuHwc if available). */
void sysinfo_battery(u8* percent, u8* flags);
