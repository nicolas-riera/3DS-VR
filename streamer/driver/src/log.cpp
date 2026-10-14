#include "log.h"

#include <windows.h>
#include <shlobj.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

#include <openvr_driver.h>

static std::mutex g_logMutex;
static FILE* g_logFile;

std::wstring AppDataDir() {
	wchar_t* path = nullptr;
	std::wstring dir;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &path))) {
		dir = std::wstring(path) + L"\\3DS VR";
		CoTaskMemFree(path);
	} else {
		dir = L".";
	}
	CreateDirectoryW(dir.c_str(), nullptr);
	return dir;
}

void LogInit() {
	std::lock_guard<std::mutex> lock(g_logMutex);
	if (g_logFile) return;
	std::wstring path = AppDataDir() + L"\\driver.log";
	// keep the last session's log: it explains what went wrong before a restart
	std::wstring previous = path.substr(0, path.size() - 4) + L".previous.log";
	MoveFileExW(path.c_str(), previous.c_str(), MOVEFILE_REPLACE_EXISTING);
	g_logFile = _wfopen(path.c_str(), L"w");
}

void Log(const char* fmt, ...) {
	char msg[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);

	if (vr::VRDriverLog()) {
		char line[1100];
		snprintf(line, sizeof(line), "[3DS VR] %s\n", msg);
		vr::VRDriverLog()->Log(line);
	}
	std::lock_guard<std::mutex> lock(g_logMutex);
	if (g_logFile) {
		SYSTEMTIME t;
		GetLocalTime(&t);
		fprintf(g_logFile, "%02d:%02d:%02d.%03d %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, msg);
		fflush(g_logFile);
	}
}

uint64_t NowUs() {
	static LARGE_INTEGER freq = [] {
		LARGE_INTEGER f;
		QueryPerformanceFrequency(&f);
		return f;
	}();
	LARGE_INTEGER c;
	QueryPerformanceCounter(&c);
	return (uint64_t)(c.QuadPart / freq.QuadPart) * 1000000ull + (uint64_t)(c.QuadPart % freq.QuadPart) * 1000000ull / freq.QuadPart;
}
