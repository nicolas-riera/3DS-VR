// 3DS VR app: the only thing the user starts on the PC.
//  - registers the SteamVR driver that ships next to it,
//  - offers to open the firewall port and to install the gamepad driver (Xbox
//    controller), and runs elevated as "3DS VR.exe --install-gamepad" for that,
//  - starts SteamVR (the driver only activates while this app runs),
//  - shows the pairing requests and the stream status.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <objbase.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace Gdiplus {
using std::max;
using std::min;
} // namespace Gdiplus
#include <gdiplus.h>

#include <ViGEm/Client.h>

#include "gamepad_setup.h"
#include "ipc.h"
#include "resource.h"
#include "vr3ds_pad.h"
#include "vr3ds_protocol.h"

#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;

// ------------------------------------------------------------------ state

enum Task { TASK_NONE, TASK_FIREWALL, TASK_PAD_INSTALL, TASK_PAD_REMOVE, TASK_RESTART_STEAMVR };

enum : UINT {
	WM_APP_TASK_DONE = WM_APP + 1,
	WM_APP_SETUP_DONE,
};

enum ButtonId {
	BTN_PAIR = 1,
	BTN_REFUSE,
	BTN_STEAMVR,
	BTN_FIREWALL,
	BTN_PAD,
	BTN_FORGET,
};

struct Button {
	ButtonId id;
	RectF rect;
	std::wstring label;
	bool primary;
};

static HWND g_hwnd;
static HICON g_iconLarge;
static float g_scale = 1.0f;
static SOCKET g_ipc = INVALID_SOCKET;
static IpcStatus g_status;
static DWORD g_lastStatusTick;
static bool g_hasStatus;
static uint64_t g_flashedPairId;

static std::wstring g_exeDir, g_driverDir, g_runtimeDir;
static bool g_runtimeFound;
static bool g_driverRegistered;
static bool g_firewallOk;
static bool g_vigemOk;
static bool g_padRunning, g_padInstalled; // the 3DS VR gamepad driver
static bool g_padReboot;
static bool g_steamvrRunning;
static bool g_steamvrLaunched;
static DWORD g_steamvrStartTick;
static std::atomic<Task> g_task{TASK_NONE};
static std::wstring g_taskError;

static std::vector<Button> g_buttons;
static int g_hover = -1;
static int g_pressed = -1;
static bool g_forgetArmed;
static DWORD g_forgetArmedTick;
static bool g_padRemoveArmed;
static DWORD g_padRemoveArmedTick;

static const wchar_t* const kModelNames[] = {L"Nintendo 3DS", L"Nintendo 3DS XL", L"New Nintendo 3DS",
											   L"Nintendo 2DS", L"New Nintendo 3DS XL", L"New Nintendo 2DS XL"};

static const wchar_t* ModelName(uint8_t m) {
	return m < VR3DS_MODEL_COUNT ? kModelNames[m] : L"Nintendo 3DS";
}

static std::wstring Widen(const char* s, size_t max) {
	std::string n(s, strnlen(s, max));
	int len = MultiByteToWideChar(CP_UTF8, 0, n.c_str(), -1, nullptr, 0);
	std::wstring w(len > 0 ? len - 1 : 0, L'\0');
	if (len > 1) MultiByteToWideChar(CP_UTF8, 0, n.c_str(), -1, w.data(), len);
	return w;
}

static bool DriverAlive() {
	return g_hasStatus && GetTickCount() - g_lastStatusTick < 2500;
}

// ------------------------------------------------------------------ setup

static std::wstring ReadFileUtf8(const std::wstring& path) {
	FILE* f = _wfopen(path.c_str(), L"rb");
	if (!f) return {};
	std::string data;
	char buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
	fclose(f);
	return Widen(data.c_str(), data.size());
}

// Strings of a JSON array value: just enough for openvrpaths.vrpath.
static std::vector<std::wstring> JsonStringArray(const std::wstring& json, const wchar_t* key) {
	std::vector<std::wstring> out;
	size_t k = json.find(std::wstring(L"\"") + key + L"\"");
	if (k == std::wstring::npos) return out;
	size_t open = json.find(L'[', k), close = json.find(L']', k);
	if (open == std::wstring::npos || close == std::wstring::npos || close < open) return out;
	size_t i = open;
	while (true) {
		size_t q0 = json.find(L'"', i + 1);
		if (q0 == std::wstring::npos || q0 > close) break;
		std::wstring s;
		size_t j = q0 + 1;
		for (; j < json.size() && json[j] != L'"'; j++) {
			if (json[j] == L'\\' && j + 1 < json.size()) j++;
			s += json[j];
		}
		out.push_back(s);
		i = j;
	}
	return out;
}

static std::wstring NormalizePath(std::wstring p) {
	std::replace(p.begin(), p.end(), L'/', L'\\');
	while (!p.empty() && p.back() == L'\\') p.pop_back();
	std::transform(p.begin(), p.end(), p.begin(), ::towlower);
	return p;
}

static DWORD RunHidden(const std::wstring& exe, const std::wstring& args, DWORD timeoutMs = 15000) {
	std::wstring cmd = L"\"" + exe + L"\" " + args;
	STARTUPINFOW si = {sizeof(si)};
	si.dwFlags = STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;
	PROCESS_INFORMATION pi = {};
	if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
		return (DWORD)-1;
	WaitForSingleObject(pi.hProcess, timeoutMs);
	DWORD code = (DWORD)-1;
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return code;
}

static DWORD RunElevated(const std::wstring& exe, const std::wstring& args, DWORD timeoutMs = 120000) {
	SHELLEXECUTEINFOW sei = {sizeof(sei)};
	sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
	sei.lpVerb = L"runas";
	sei.lpFile = exe.c_str();
	sei.lpParameters = args.c_str();
	sei.nShow = SW_HIDE;
	if (!ShellExecuteExW(&sei) || !sei.hProcess) return (DWORD)-1;
	WaitForSingleObject(sei.hProcess, timeoutMs);
	DWORD code = (DWORD)-1;
	GetExitCodeProcess(sei.hProcess, &code);
	CloseHandle(sei.hProcess);
	return code;
}

static std::wstring SystemExe(const wchar_t* name) {
	wchar_t dir[MAX_PATH];
	GetSystemDirectoryW(dir, MAX_PATH);
	return std::wstring(dir) + L"\\" + name;
}

static std::wstring OpenVrPathsFile() {
	wchar_t* local = nullptr;
	size_t len = 0;
	_wdupenv_s(&local, &len, L"LOCALAPPDATA");
	std::wstring p = local ? std::wstring(local) + L"\\openvr\\openvrpaths.vrpath" : L"";
	free(local);
	return p;
}

// Registers driver\3dsvr with SteamVR (and removes copies registered from elsewhere).
static void RegisterDriver() {
	std::wstring json = ReadFileUtf8(OpenVrPathsFile());
	std::vector<std::wstring> runtime = JsonStringArray(json, L"runtime");
	g_runtimeFound = false;
	for (const std::wstring& r : runtime) {
		if (GetFileAttributesW((r + L"\\bin\\win64\\vrpathreg.exe").c_str()) != INVALID_FILE_ATTRIBUTES) {
			g_runtimeDir = r;
			g_runtimeFound = true;
			break;
		}
	}
	if (!g_runtimeFound) return;
	std::wstring vrpathreg = g_runtimeDir + L"\\bin\\win64\\vrpathreg.exe";
	std::wstring mine = NormalizePath(g_driverDir);
	bool present = false;
	for (const std::wstring& d : JsonStringArray(json, L"external_drivers")) {
		std::wstring n = NormalizePath(d);
		if (n == mine) present = true;
		else if (n.size() >= 6 && n.compare(n.size() - 6, 6, L"\\3dsvr") == 0)
			RunHidden(vrpathreg, L"removedriver \"" + d + L"\""); // an older copy of 3DS VR
	}
	if (!present) RunHidden(vrpathreg, L"adddriver \"" + g_driverDir + L"\"");
	json = ReadFileUtf8(OpenVrPathsFile());
	g_driverRegistered = false;
	for (const std::wstring& d : JsonStringArray(json, L"external_drivers"))
		if (NormalizePath(d) == mine) g_driverRegistered = true;
}

// Removes driver\3dsvr from SteamVR when the app closes: without the app,
// SteamVR does not even see 3DS VR (it is registered again at the next start).
static void UnregisterDriver() {
	if (!g_runtimeFound || !g_driverRegistered) return;
	RunHidden(g_runtimeDir + L"\\bin\\win64\\vrpathreg.exe", L"removedriver \"" + g_driverDir + L"\"");
	g_driverRegistered = false;
}

static bool CheckFirewall() {
	return RunHidden(SystemExe(L"netsh.exe"), L"advfirewall firewall show rule name=\"3DS VR\"") == 0;
}

// The fallback when the 3DS VR gamepad driver is not installed. Connects to the
// bus like the driver does: ViGEmBus can be installed under other names
// (Virtual Desktop ships it as "Virtual Desktop Gamepad Emulation Bus").
static bool CheckVigem() {
	PVIGEM_CLIENT client = vigem_alloc();
	if (!client) return false;
	bool ok = VIGEM_SUCCESS(vigem_connect(client));
	if (ok) vigem_disconnect(client);
	vigem_free(client);
	return ok;
}

static bool ProcessRunning(const wchar_t* name) {
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE) return false;
	PROCESSENTRY32W pe = {sizeof(pe)};
	bool found = false;
	for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
		if (!_wcsicmp(pe.szExeFile, name)) {
			found = true;
			break;
		}
	CloseHandle(snap);
	return found;
}

static void KillProcess(const wchar_t* name) {
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE) return;
	PROCESSENTRY32W pe = {sizeof(pe)};
	for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
		if (_wcsicmp(pe.szExeFile, name)) continue;
		HANDLE p = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID);
		if (p) {
			TerminateProcess(p, 0);
			WaitForSingleObject(p, 3000);
			CloseHandle(p);
		}
	}
	CloseHandle(snap);
}

static void LaunchSteamVR() {
	ShellExecuteW(nullptr, L"open", L"steam://rungameid/250820", nullptr, nullptr, SW_SHOWNORMAL);
	g_steamvrLaunched = true;
	g_steamvrStartTick = GetTickCount();
}

static void RunTask(Task t) {
	Task expected = TASK_NONE;
	if (!g_task.compare_exchange_strong(expected, t)) return;
	g_taskError.clear();
	std::thread([t] {
		if (t == TASK_FIREWALL) {
			DWORD code = RunElevated(SystemExe(L"netsh.exe"),
									 L"advfirewall firewall add rule name=\"3DS VR\" dir=in action=allow protocol=UDP "
									 L"localport=" + std::to_wstring(VR3DS_PORT_STREAMER) + L" profile=any");
			if (code != 0) g_taskError = L"The firewall rule was not added.";
		} else if (t == TASK_PAD_INSTALL || t == TASK_PAD_REMOVE) {
			// this exe again, elevated: one admin prompt for the whole job
			bool install = t == TASK_PAD_INSTALL;
			wchar_t exe[MAX_PATH];
			GetModuleFileNameW(nullptr, exe, MAX_PATH);
			std::wstring inf = g_exeDir + L"\\gamepad\\" VR3DS_PAD_INF_NAME;
			if (install && GetFileAttributesW(inf.c_str()) == INVALID_FILE_ATTRIBUTES) {
				g_taskError = L"The gamepad folder is missing.";
			} else {
				DWORD code = RunElevated(exe, install ? PAD_ARG_INSTALL : PAD_ARG_REMOVE, 300000);
				if (code == ERROR_SUCCESS_REBOOT_REQUIRED) g_padReboot = true;
				else if (code == (DWORD)-1)
					g_taskError = install ? L"The driver was not installed." : L"The driver was not removed.";
				else if (code != ERROR_SUCCESS) {
					wchar_t msg[64];
					swprintf(msg, 64, install ? L"Install failed (0x%08X)" : L"Removal failed (0x%08X)", code);
					g_taskError = msg;
				}
			}
		} else if (t == TASK_RESTART_STEAMVR) {
			for (const wchar_t* p : {L"vrmonitor.exe", L"vrdashboard.exe", L"vrwebhelper.exe", L"vrcompositor.exe",
									 L"vrserver.exe", L"vrstartup.exe"})
				KillProcess(p);
			Sleep(1500);
			LaunchSteamVR();
		}
		PostMessageW(g_hwnd, WM_APP_TASK_DONE, (WPARAM)t, 0);
	}).detach();
}

static void SetupThread() {
	RegisterDriver();
	g_firewallOk = CheckFirewall();
	g_vigemOk = CheckVigem();
	g_padRunning = PadDriverRunning();
	g_padInstalled = PadDriverInstalled();
	PostMessageW(g_hwnd, WM_APP_SETUP_DONE, 0, 0);
}

// -------------------------------------------------------------------- IPC

static void IpcInit() {
	g_ipc = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	sockaddr_in a = {};
	a.sin_family = AF_INET;
	a.sin_port = htons(VR3DS_IPC_APP_PORT);
	inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
	if (bind(g_ipc, (sockaddr*)&a, sizeof(a)) != 0) {
		closesocket(g_ipc);
		g_ipc = INVALID_SOCKET;
		return;
	}
	u_long nb = 1;
	ioctlsocket(g_ipc, FIONBIO, &nb);
}

static void IpcSend(const void* data, int len) {
	if (g_ipc == INVALID_SOCKET) return;
	sockaddr_in a = {};
	a.sin_family = AF_INET;
	a.sin_port = htons(VR3DS_IPC_DRIVER_PORT);
	inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
	sendto(g_ipc, (const char*)data, len, 0, (sockaddr*)&a, sizeof(a));
}

static bool IpcPoll() {
	bool changed = false;
	IpcStatus st;
	while (g_ipc != INVALID_SOCKET) {
		int n = recv(g_ipc, (char*)&st, sizeof(st), 0);
		if (n <= 0) break;
		if (n < (int)sizeof(st) || st.magic != VR3DS_IPC_MAGIC || st.type != IPC_STATUS) continue;
		changed |= !g_hasStatus || memcmp(&st, &g_status, sizeof(st)) != 0;
		g_status = st;
		g_hasStatus = true;
		g_lastStatusTick = GetTickCount();
	}
	return changed;
}

// ---------------------------------------------------------------- drawing

static const wchar_t kSep[] = L"  \u00B7  "; // between the items of a status line

static const Color kBg(255, 15, 17, 27);
static const Color kCard(255, 27, 31, 48);
static const Color kCardHi(255, 37, 43, 66);
static const Color kText(255, 236, 239, 247);
static const Color kDim(255, 140, 150, 178);
static const Color kAccent(255, 92, 168, 255);
static const Color kGreen(255, 96, 222, 146);
static const Color kOrange(255, 255, 184, 82);
static const Color kRed(255, 255, 98, 104);

static float S(float v) {
	return v * g_scale;
}

static void FillRound(Graphics& g, const Color& c, RectF r, float rad) {
	GraphicsPath path;
	float d = rad * 2;
	path.AddArc(r.X, r.Y, d, d, 180, 90);
	path.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
	path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
	path.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
	path.CloseFigure();
	SolidBrush b(c);
	g.FillPath(&b, &path);
}

static void Text(Graphics& g, const std::wstring& s, float x, float y, float size, const Color& c, bool bold = false,
				 float width = 0, StringAlignment align = StringAlignmentNear) {
	FontFamily fam(L"Segoe UI");
	Font font(&fam, S(size), bold ? FontStyleBold : FontStyleRegular, UnitPixel);
	SolidBrush b(c);
	StringFormat fmt;
	fmt.SetAlignment(align);
	fmt.SetTrimming(StringTrimmingEllipsisCharacter);
	RectF r(S(x), S(y), width > 0 ? S(width) : 10000.0f, S(size * 2.2f));
	g.DrawString(s.c_str(), -1, &font, r, &fmt, &b);
}

static void Dot(Graphics& g, float x, float y, const Color& c) {
	SolidBrush b(c);
	g.FillEllipse(&b, S(x), S(y), S(10), S(10));
}

static void AddButton(ButtonId id, float x, float y, float w, float h, const std::wstring& label, bool primary) {
	g_buttons.push_back({id, RectF(S(x), S(y), S(w), S(h)), label, primary});
}

static void DrawButtons(Graphics& g) {
	FontFamily fam(L"Segoe UI");
	Font font(&fam, S(13), FontStyleBold, UnitPixel);
	StringFormat fmt;
	fmt.SetAlignment(StringAlignmentCenter);
	fmt.SetLineAlignment(StringAlignmentCenter);
	for (size_t i = 0; i < g_buttons.size(); i++) {
		const Button& b = g_buttons[i];
		bool hover = (int)i == g_hover, pressed = (int)i == g_pressed;
		Color bg = b.primary ? (pressed ? Color(255, 70, 140, 230) : hover ? Color(255, 120, 186, 255) : kAccent)
							 : (pressed ? Color(255, 30, 35, 52) : hover ? Color(255, 58, 66, 100) : kCardHi);
		FillRound(g, bg, b.rect, S(7));
		SolidBrush tb(b.primary ? Color(255, 10, 14, 26) : kText);
		g.DrawString(b.label.c_str(), -1, &font, b.rect, &fmt, &tb);
	}
}

static void SetupRow(Graphics& g, float y, const Color& dot, const std::wstring& title, const std::wstring& detail,
					 ButtonId btn = (ButtonId)0, const std::wstring& label = L"") {
	Dot(g, 34, y + 9, dot);
	Text(g, title, 54, y, 14, kText, true, btn ? 250.0f : 380.0f);
	Text(g, detail, 54, y + 19, 12, kDim, false, btn ? 250.0f : 380.0f);
	if (btn) AddButton(btn, 318, y + 4, 128, 30, label, false);
}

static void Paint(HDC hdc, const RECT& rc) {
	g_buttons.clear();
	Graphics g(hdc);
	g.SetSmoothingMode(SmoothingModeAntiAlias);
	g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
	SolidBrush bg(kBg);
	g.FillRectangle(&bg, 0, 0, rc.right, rc.bottom);

	// header
	if (g_iconLarge) DrawIconEx(hdc, (int)S(20), (int)S(16), g_iconLarge, (int)S(44), (int)S(44), 0, nullptr, DI_NORMAL);
	Text(g, L"3DS VR", 74, 14, 22, kText, true);
	bool streaming = DriverAlive() && g_status.state == IPC_STATE_STREAMING;
	std::wstring sub;
	Color subColor = kDim;
	if (!g_runtimeFound) sub = L"SteamVR is not installed", subColor = kRed;
	else if (!DriverAlive()) sub = g_steamvrRunning ? L"Waiting for SteamVR..." : L"SteamVR is not running";
	else if (streaming) sub = L"Streaming to " + Widen(g_status.consoleName, 32), subColor = kGreen;
	else sub = L"Ready: start 3DS VR on your console";
	Text(g, sub, 75, 44, 13, subColor);

	// console / pairing card
	RectF card(S(16), S(78), S(448), S(126));
	FillRound(g, kCard, card, S(10));
	if (DriverAlive() && g_status.pairPending) {
		Text(g, L"New console", 32, 88, 12, kAccent, true);
		Text(g, Widen(g_status.pairName, 32) + kSep + ModelName(g_status.pairModel), 32, 106, 15, kText, true, 260);
		Text(g, L"Pair it if the console shows the same code.", 32, 132, 12, kDim, false, 270);
		wchar_t code[8];
		swprintf(code, 8, L"%04u", g_status.pairCode);
		Text(g, code, 300, 92, 34, kText, true, 150, StringAlignmentCenter);
		AddButton(BTN_PAIR, 300, 158, 72, 32, L"Pair", true);
		AddButton(BTN_REFUSE, 380, 158, 72, 32, L"Refuse", false);
	} else if (streaming) {
		const IpcStatus& s = g_status;
		Text(g, ModelName(s.model), 32, 88, 12, kAccent, true);
		Text(g, Widen(s.consoleName, 32), 32, 104, 17, kText, true, 260);
		wchar_t line[200];
		swprintf(line, 200, L"%s%s%ux%u per eye%s%s%s%u fps", s.stereo ? L"3D" : L"2D", kSep, s.eyeWidth,
				 s.eyeHeight, kSep, s.codec == VR3DS_CODEC_H264 ? L"H.264" : L"JPEG", kSep, s.fps);
		Text(g, line, 32, 134, 12, kDim, false, 420);
		swprintf(line, 200, L"%.0f fps sent%s%.1f Mbps%slatency %.0f ms%s%s", s.encodeFps, kSep, s.sentMbps, kSep,
				 s.latencyMs, kSep, Widen(s.consoleIp, 16).c_str());
		Text(g, line, 32, 154, 12, kDim, false, 420);
		Text(g, Widen(s.encoder, 48), 32, 174, 11, Color(255, 100, 108, 135), false, 420);
		wchar_t batt[32];
		swprintf(batt, 32, L"%u%%", s.battery);
		Color bc = s.battery <= 15 ? kRed : s.battery <= 35 ? kOrange : kGreen;
		Text(g, batt, 352, 102, 17, bc, true, 96, StringAlignmentFar);
		Text(g, (s.batteryFlags & VR3DS_BATT_CHARGING) ? L"charging" : L"battery", 352, 124, 11, kDim, false, 96,
			 StringAlignmentFar);
	} else {
		Text(g, L"No console connected", 32, 90, 17, kText, true);
		Text(g, L"1. Start 3DS VR on your 3DS (same Wi-Fi network as this PC).", 32, 120, 12, kDim, false, 420);
		Text(g, L"2. The first time, check the code and click Pair here.", 32, 140, 12, kDim, false, 420);
		Text(g, L"3. Put on your 3DS and play. SELECT + START recenters the view.", 32, 160, 12, kDim, false, 420);
	}

	// setup checklist
	Text(g, L"SETUP", 20, 216, 11, kDim, true);
	float y = 236;
	if (!g_runtimeFound) {
		SetupRow(g, y, kRed, L"SteamVR not found", L"Install SteamVR from Steam, then restart this app.");
	} else if (DriverAlive()) {
		SetupRow(g, y, kGreen, L"SteamVR", L"Running with the 3DS VR headset.");
	} else if (g_steamvrRunning && g_steamvrLaunched && GetTickCount() - g_steamvrStartTick < 20000) {
		SetupRow(g, y, kOrange, L"SteamVR", L"Starting...");
	} else if (g_steamvrRunning) {
		SetupRow(g, y, kOrange, L"SteamVR", L"Running without 3DS VR: restart it.", BTN_STEAMVR,
				 g_task == TASK_RESTART_STEAMVR ? L"Restarting..." : L"Restart SteamVR");
	} else {
		SetupRow(g, y, kOrange, L"SteamVR", g_driverRegistered ? L"Not running." : L"Driver not registered yet.",
				 BTN_STEAMVR, L"Start SteamVR");
	}
	y += 46;
	if (g_firewallOk) SetupRow(g, y, kGreen, L"Network", L"Consoles can reach this PC.");
	else
		SetupRow(g, y, kOrange, L"Network", L"Windows Firewall may block consoles.", BTN_FIREWALL,
				 g_task == TASK_FIREWALL ? L"Waiting..." : L"Allow");
	y += 46;
	// the 3DS VR gamepad driver, or ViGEmBus when it is already there
	ButtonId padBtn = g_padInstalled ? BTN_PAD : (ButtonId)0;
	const wchar_t* padLabel = g_padRemoveArmed ? L"Click again" : L"Remove";
	if (g_task == TASK_PAD_INSTALL || g_task == TASK_PAD_REMOVE)
		SetupRow(g, y, kOrange, L"Xbox controller",
				 g_task == TASK_PAD_INSTALL ? L"Installing the gamepad driver..." : L"Removing the gamepad driver...");
	else if (g_padReboot)
		SetupRow(g, y, kOrange, L"Xbox controller", L"Restart Windows to finish.");
	else if (g_padInstalled && !g_padRunning)
		SetupRow(g, y, kRed, L"Xbox controller", L"The gamepad driver does not start.", padBtn, padLabel);
	else if (!g_padRunning && !g_vigemOk)
		SetupRow(g, y, kOrange, L"Xbox controller", L"Needs the 3DS VR gamepad driver.", BTN_PAD, L"Install");
	else if (streaming && g_status.gamepad == IPC_PAD_OK)
		SetupRow(g, y, kGreen, L"Xbox controller", L"Connected as an Xbox 360 controller.", padBtn, padLabel);
	else if (streaming && g_status.gamepad != IPC_PAD_OK)
		SetupRow(g, y, kRed, L"Xbox controller", L"Could not create the virtual controller.", padBtn, padLabel);
	else
		SetupRow(g, y, kGreen, L"Xbox controller",
				 g_padRunning ? L"Ready (appears when a console connects)." : L"Ready with ViGEmBus.", padBtn, padLabel);
	y += 46;
	if (streaming)
		SetupRow(g, y, g_status.audio ? kGreen : kOrange, L"Sound",
				 g_status.audio ? L"PC sound plays on the console." : L"No output device to capture.");
	else
		SetupRow(g, y, kGreen, L"Sound", L"PC sound goes to the console while streaming.");

	// footer
	float fy = 428;
	unsigned paired = DriverAlive() ? g_status.pairedCount : 0;
	wchar_t foot[64];
	swprintf(foot, 64, L"%u paired console%s", paired, paired == 1 ? L"" : L"s");
	Text(g, foot, 20, fy + 6, 12, kDim);
	if (DriverAlive() && paired > 0)
		AddButton(BTN_FORGET, 164, fy, 110, 28, g_forgetArmed ? L"Click again" : L"Forget all", false);
	if (!g_taskError.empty()) Text(g, g_taskError, 286, fy + 6, 12, kRed, false, 180, StringAlignmentFar);
	else Text(g, L"v1.0.0", 286, fy + 6, 12, Color(255, 90, 98, 125), false, 180, StringAlignmentFar);

	DrawButtons(g);
}

// ------------------------------------------------------------------ input

static int HitButton(int x, int y) {
	for (size_t i = 0; i < g_buttons.size(); i++)
		if (g_buttons[i].rect.Contains((REAL)x, (REAL)y)) return (int)i;
	return -1;
}

static void OnButton(ButtonId id) {
	switch (id) {
	case BTN_PAIR:
	case BTN_REFUSE: {
		IpcPairDecision d = {};
		d.magic = VR3DS_IPC_MAGIC;
		d.type = IPC_PAIR_DECISION;
		d.accept = id == BTN_PAIR;
		d.consoleId = g_status.pairConsoleId;
		IpcSend(&d, sizeof(d));
		g_status.pairPending = 0;
		break;
	}
	case BTN_STEAMVR:
		if (g_steamvrRunning) RunTask(TASK_RESTART_STEAMVR);
		else {
			if (!g_driverRegistered) RegisterDriver();
			LaunchSteamVR();
		}
		break;
	case BTN_FIREWALL:
		RunTask(TASK_FIREWALL);
		break;
	case BTN_PAD:
		if (!g_padInstalled) {
			RunTask(TASK_PAD_INSTALL);
		} else if (g_padRemoveArmed && GetTickCount() - g_padRemoveArmedTick < 4000) {
			g_padRemoveArmed = false;
			RunTask(TASK_PAD_REMOVE);
		} else {
			g_padRemoveArmed = true;
			g_padRemoveArmedTick = GetTickCount();
		}
		break;
	case BTN_FORGET:
		if (g_forgetArmed && GetTickCount() - g_forgetArmedTick < 4000) {
			IpcCommand c = {};
			c.magic = VR3DS_IPC_MAGIC;
			c.type = IPC_FORGET_ALL;
			IpcSend(&c, sizeof(c));
			g_forgetArmed = false;
		} else {
			g_forgetArmed = true;
			g_forgetArmedTick = GetTickCount();
		}
		break;
	}
	InvalidateRect(g_hwnd, nullptr, FALSE);
}

// ----------------------------------------------------------------- window

static void ResizeForDpi(HWND hwnd, UINT dpi) {
	g_scale = dpi / 96.0f;
	RECT r = {0, 0, (LONG)S(480), (LONG)S(470)};
	AdjustWindowRectExForDpi(&r, GetWindowLongW(hwnd, GWL_STYLE), FALSE, 0, dpi);
	SetWindowPos(hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
	switch (msg) {
	case WM_CREATE: {
		g_hwnd = hwnd;
		BOOL dark = TRUE;
		DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
		ResizeForDpi(hwnd, GetDpiForWindow(hwnd));
		SetTimer(hwnd, 1, 100, nullptr);
		SetTimer(hwnd, 2, 1000, nullptr);
		std::thread(SetupThread).detach();
		return 0;
	}
	case WM_DPICHANGED: {
		g_scale = HIWORD(wp) / 96.0f;
		RECT* r = (RECT*)lp;
		SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER);
		InvalidateRect(hwnd, nullptr, FALSE);
		return 0;
	}
	case WM_TIMER:
		if (wp == 1) {
			bool wasAlive = DriverAlive();
			bool changed = IpcPoll();
			if (wasAlive != DriverAlive()) changed = true;
			if (DriverAlive() && g_status.pairPending && g_status.pairConsoleId != g_flashedPairId) {
				g_flashedPairId = g_status.pairConsoleId;
				FLASHWINFO fi = {sizeof(fi), hwnd, FLASHW_ALL | FLASHW_TIMERNOFG, 5, 0};
				FlashWindowEx(&fi);
				ShowWindow(hwnd, SW_RESTORE);
				SetForegroundWindow(hwnd);
			}
			if (changed) InvalidateRect(hwnd, nullptr, FALSE);
		} else {
			g_steamvrRunning = ProcessRunning(L"vrserver.exe");
			if (g_forgetArmed && GetTickCount() - g_forgetArmedTick > 4000) g_forgetArmed = false;
			if (g_padRemoveArmed && GetTickCount() - g_padRemoveArmedTick > 4000) g_padRemoveArmed = false;
			if (g_padInstalled && !g_padRunning && g_task == TASK_NONE) g_padRunning = PadDriverRunning();
			InvalidateRect(hwnd, nullptr, FALSE);
		}
		return 0;
	case WM_APP_SETUP_DONE:
		g_steamvrRunning = ProcessRunning(L"vrserver.exe");
		// the driver only loads if SteamVR starts after this app
		if (g_runtimeFound && !g_steamvrRunning) LaunchSteamVR();
		InvalidateRect(hwnd, nullptr, FALSE);
		return 0;
	case WM_APP_TASK_DONE:
		if ((Task)wp == TASK_FIREWALL) g_firewallOk = CheckFirewall();
		if ((Task)wp == TASK_PAD_INSTALL || (Task)wp == TASK_PAD_REMOVE) {
			g_padRunning = PadDriverRunning();
			g_padInstalled = PadDriverInstalled();
		}
		g_task = TASK_NONE;
		InvalidateRect(hwnd, nullptr, FALSE);
		return 0;
	case WM_MOUSEMOVE: {
		int h = HitButton(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
		if (h != g_hover) {
			g_hover = h;
			InvalidateRect(hwnd, nullptr, FALSE);
		}
		TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
		TrackMouseEvent(&tme);
		SetCursor(LoadCursorW(nullptr, h >= 0 ? IDC_HAND : IDC_ARROW));
		return 0;
	}
	case WM_MOUSELEAVE:
		g_hover = -1;
		InvalidateRect(hwnd, nullptr, FALSE);
		return 0;
	case WM_LBUTTONDOWN:
		g_pressed = HitButton(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
		SetCapture(hwnd);
		InvalidateRect(hwnd, nullptr, FALSE);
		return 0;
	case WM_LBUTTONUP: {
		ReleaseCapture();
		int h = HitButton(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
		int p = g_pressed;
		g_pressed = -1;
		if (p >= 0 && p == h) OnButton(g_buttons[p].id);
		InvalidateRect(hwnd, nullptr, FALSE);
		return 0;
	}
	case WM_SETCURSOR:
		if (LOWORD(lp) == HTCLIENT) return TRUE; // set in WM_MOUSEMOVE
		break;
	case WM_ERASEBKGND:
		return 1;
	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC hdc = BeginPaint(hwnd, &ps);
		RECT rc;
		GetClientRect(hwnd, &rc);
		HDC mem = CreateCompatibleDC(hdc);
		HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
		HGDIOBJ old = SelectObject(mem, bmp);
		Paint(mem, rc);
		BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
		SelectObject(mem, old);
		DeleteObject(bmp);
		DeleteDC(mem);
		EndPaint(hwnd, &ps);
		return 0;
	}
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
	// elevated helper started by the app itself: install or remove the gamepad
	// driver, the exit code is the result
	int argc = 0;
	LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	bool padInstall = argc > 1 && !wcscmp(argv[1], PAD_ARG_INSTALL);
	bool padRemove = argc > 1 && !wcscmp(argv[1], PAD_ARG_REMOVE);
	LocalFree(argv);
	if (padInstall || padRemove) {
		wchar_t self[MAX_PATH];
		GetModuleFileNameW(nullptr, self, MAX_PATH);
		std::wstring dir = self;
		dir = dir.substr(0, dir.find_last_of(L'\\'));
		return (int)(padInstall ? InstallPadDriver(dir + L"\\gamepad") : RemovePadDriver());
	}

	// one instance: it is also what tells the driver to activate
	HANDLE mutex = CreateMutexW(nullptr, TRUE, VR3DS_APP_MUTEX);
	if (GetLastError() == ERROR_ALREADY_EXISTS) {
		HWND other = FindWindowW(L"3DSVRApp", nullptr);
		if (other) {
			ShowWindow(other, SW_RESTORE);
			SetForegroundWindow(other);
		}
		return 0;
	}

	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
	GdiplusStartupInput gsi;
	ULONG_PTR gdipToken;
	GdiplusStartup(&gdipToken, &gsi, nullptr);

	wchar_t exe[MAX_PATH];
	GetModuleFileNameW(nullptr, exe, MAX_PATH);
	g_exeDir = exe;
	g_exeDir = g_exeDir.substr(0, g_exeDir.find_last_of(L'\\'));
	g_driverDir = g_exeDir + L"\\driver\\3dsvr";
	IpcInit();

	g_iconLarge = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, 256, 256, 0);
	WNDCLASSEXW wc = {sizeof(wc)};
	wc.lpfnWndProc = WndProc;
	wc.hInstance = inst;
	wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	wc.hIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, 32, 32, 0);
	wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, 16, 16, 0);
	wc.lpszClassName = L"3DSVRApp";
	RegisterClassExW(&wc);

	HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"3DS VR", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
								CW_USEDEFAULT, CW_USEDEFAULT, 480, 470, nullptr, nullptr, inst, nullptr);
	ShowWindow(hwnd, show);
	UpdateWindow(hwnd);

	MSG msg;
	while (GetMessageW(&msg, nullptr, 0, 0)) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}

	UnregisterDriver();
	if (g_ipc != INVALID_SOCKET) closesocket(g_ipc);
	GdiplusShutdown(gdipToken);
	WSACleanup();
	CoUninitialize();
	ReleaseMutex(mutex);
	CloseHandle(mutex);
	return 0;
}
