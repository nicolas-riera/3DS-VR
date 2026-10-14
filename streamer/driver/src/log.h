#pragma once
#include <cstdint>
#include <string>

// Logs to SteamVR's vrserver.txt and to %APPDATA%\3DS VR\driver.log.
void LogInit();
void Log(const char* fmt, ...);

// %APPDATA%\3DS VR (created on demand).
std::wstring AppDataDir();

// Microseconds on a monotonic clock.
uint64_t NowUs();
