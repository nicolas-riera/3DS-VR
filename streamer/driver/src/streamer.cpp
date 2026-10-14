#include "streamer.h"

#include <mstcpip.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>

#include "log.h"

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

static constexpr uint64_t kSessionTimeoutUs = 3000000;
static constexpr uint64_t kConfigResendUs = 1000000;
static constexpr uint64_t kPendingTimeoutUs = 5000000;
static constexpr uint64_t kRejectUs = 10000000;
static constexpr uint64_t kSettingsDebounceUs = 250000;
static constexpr uint64_t kStatusPeriodUs = 500000;
static constexpr float kOverscanY = 1.15f; // margin around the visible picture for reprojection
static constexpr float kMaxIpd = 0.064f;
static constexpr float kEyeHeightRatio = 0.935f; // eye height / body height

static uint32_t Random32() {
	thread_local std::mt19937 rng{std::random_device{}()};
	return rng();
}

static std::string AddrStr(const sockaddr_in& a) {
	char b[32] = {};
	inet_ntop(AF_INET, &a.sin_addr, b, sizeof(b));
	return b;
}

static std::string FixedName(const char* src, size_t n) {
	return std::string(src, strnlen(src, n));
}

// UTF-8 names are cut between characters, never inside one.
static void CopyName(char* dst, size_t n, const std::string& s) {
	memset(dst, 0, n);
	size_t len = std::min(n - 1, s.size());
	if (len < s.size())
		while (len > 0 && ((uint8_t)s[len] & 0xC0) == 0x80) len--;
	memcpy(dst, s.data(), len);
}

static bool SameHost(const sockaddr_in& a, const sockaddr_in& b) {
	return a.sin_addr.s_addr == b.sin_addr.s_addr;
}

// ------------------------------------------------------------------ setup

bool Streamer::Start(Compositor* compositor, PoseCallback onPose, PadCallback onPad) {
	m_compositor = compositor;
	m_onPose = std::move(onPose);
	m_onPad = std::move(onPad);

	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
	wchar_t name[MAX_COMPUTERNAME_LENGTH + 1] = {};
	DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
	char utf8[64] = {};
	if (GetComputerNameW(name, &n) && WideCharToMultiByte(CP_UTF8, 0, name, -1, utf8, sizeof(utf8), nullptr, nullptr))
		m_pcName = utf8;
	else
		m_pcName = "PC";
	m_pairing.Load();

	m_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (m_sock == INVALID_SOCKET) return false;
	int buf = 1 << 20;
	setsockopt(m_sock, SOL_SOCKET, SO_SNDBUF, (const char*)&buf, sizeof(buf));
	setsockopt(m_sock, SOL_SOCKET, SO_RCVBUF, (const char*)&buf, sizeof(buf));
	// an ICMP "port unreachable" must not break the next recvfrom()
	BOOL off = FALSE;
	DWORD ret = 0;
	WSAIoctl(m_sock, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &ret, nullptr, nullptr);
	sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(VR3DS_PORT_STREAMER);
	addr.sin_addr.s_addr = INADDR_ANY;
	if (bind(m_sock, (sockaddr*)&addr, sizeof(addr)) != 0) {
		Log("Cannot listen on UDP port %d (error %d): is another streamer running?", VR3DS_PORT_STREAMER,
			WSAGetLastError());
		closesocket(m_sock);
		m_sock = INVALID_SOCKET;
		return false;
	}
	u_long nb = 1;
	ioctlsocket(m_sock, FIONBIO, &nb);

	m_ipcSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	sockaddr_in ipc = {};
	ipc.sin_family = AF_INET;
	ipc.sin_port = htons(VR3DS_IPC_DRIVER_PORT);
	inet_pton(AF_INET, "127.0.0.1", &ipc.sin_addr);
	if (m_ipcSock != INVALID_SOCKET && bind(m_ipcSock, (sockaddr*)&ipc, sizeof(ipc)) != 0) {
		Log("IPC port %d busy: the app will not see this driver", VR3DS_IPC_DRIVER_PORT);
		closesocket(m_ipcSock);
		m_ipcSock = INVALID_SOCKET;
	}
	if (m_ipcSock != INVALID_SOCKET) ioctlsocket(m_ipcSock, FIONBIO, &nb);

	m_running = true;
	m_statWindowUs = NowUs();
	m_netThread = std::thread(&Streamer::NetThread, this);
	m_encodeThread = std::thread(&Streamer::EncodeThread, this);
	Log("Listening for consoles on UDP %d as \"%s\"", VR3DS_PORT_STREAMER, m_pcName.c_str());
	return true;
}

void Streamer::Stop() {
	if (!m_running) return;
	m_running = false;
	m_frameCv.notify_all();
	if (m_netThread.joinable()) m_netThread.join();
	if (m_encodeThread.joinable()) m_encodeThread.join();
	EndSession("SteamVR is closing");
	{
		std::lock_guard<std::mutex> lock(m_frameMutex);
		if (m_hasQueued) m_compositor->Release(m_queued);
		m_hasQueued = false;
	}
	m_encoder.reset();
	if (m_sock != INVALID_SOCKET) closesocket(m_sock);
	if (m_ipcSock != INVALID_SOCKET) closesocket(m_ipcSock);
	m_sock = m_ipcSock = INVALID_SOCKET;
	WSACleanup();
}

// ------------------------------------------------------------- accessors

SessionView Streamer::View() {
	std::lock_guard<std::mutex> lock(m_mutex);
	SessionView v;
	v.connected = m_s.active;
	v.version = m_viewVersion;
	if (!m_s.active) return v;
	v.model = m_s.model;
	v.battery = m_s.battery;
	v.batteryFlags = m_s.batteryFlags;
	if (m_s.config.configId) {
		v.encTanX = m_s.config.encTanX;
		v.encTanY = m_s.config.encTanY;
	}
	// eye separation: the 3D slider sets the depth, 2D shows one centred view;
	// whole millimetres avoid needless updates
	float ipd = m_s.config.stereo ? kMaxIpd * m_s.stereoStrength / 255.0f : 0.0f;
	v.ipd = std::round(ipd * 1000.0f) / 1000.0f;
	int height = m_s.wanted.heightCm ? m_s.wanted.heightCm : 170;
	v.eyeHeight = height * 0.01f * kEyeHeightRatio;
	return v;
}

OutputFormat Streamer::Format() {
	std::lock_guard<std::mutex> lock(m_mutex);
	OutputFormat f;
	if (m_s.active && m_s.config.configId) {
		f.eyeWidth = m_s.config.eyeWidth;
		f.eyeHeight = m_s.config.eyeHeight;
		f.stereo = m_s.config.stereo != 0;
		f.height = (uint16_t)vr3ds_picture_height(m_s.config.codec, m_s.config.stereo, m_s.config.eyeHeight);
		f.configId = m_s.config.configId;
	}
	return f;
}

void Streamer::OnFrame(const FrameSlot& slot) {
	std::lock_guard<std::mutex> lock(m_frameMutex);
	// only the newest picture matters
	if (m_hasQueued) m_compositor->Release(m_queued);
	m_queued = slot;
	m_hasQueued = true;
	m_frameCv.notify_one();
}

// --------------------------------------------------------------- network

void Streamer::SendTo(const void* data, int len, const sockaddr_in& to) {
	if (m_sock != INVALID_SOCKET) sendto(m_sock, (const char*)data, len, 0, (const sockaddr*)&to, sizeof(to));
}

void Streamer::SendPairStatus(const sockaddr_in& to, uint8_t state, uint16_t code) {
	Vr3dsPairStatus p = {};
	vr3ds_header(&p.h, VR3DS_PKT_PAIR_STATUS);
	p.state = state;
	p.code = code;
	CopyName(p.pcName, sizeof(p.pcName), m_pcName);
	SendTo(&p, sizeof(p), to);
}

void Streamer::SendConfigLocked() {
	vr3ds_header(&m_s.config.h, VR3DS_PKT_CONFIG);
	SendTo(&m_s.config, sizeof(m_s.config), m_s.addr);
	m_s.lastConfigSentUs = NowUs();
}

void Streamer::BuildConfigLocked() {
	const Vr3dsStreamSettings& w = m_s.wanted;
	bool h264 = (m_s.caps & VR3DS_CAP_HW_H264) != 0;
	Vr3dsConfig c = m_s.config;
	c.sessionId = m_s.sessionId;
	c.codec = h264 ? VR3DS_CODEC_H264 : VR3DS_CODEC_JPEG;
	c.stereo = (w.stereo && (m_s.caps & VR3DS_CAP_STEREO)) ? 1 : 0;
	// H.264: 512x256 per eye (power-of-two texture on the console, MB aligned);
	// JPEG: smaller so that the Old 3DS can decode it in software
	c.eyeWidth = h264 ? 512 : 384;
	c.eyeHeight = h264 ? 256 : 192;
	c.fps = (w.fps == 20 || w.fps == 30 || w.fps == 60) ? w.fps : (h264 ? 60 : 30);
	c.bitrateKbps = (uint16_t)std::clamp<int>(w.bitrateKbps, 500, 20000);
	float tanY = std::clamp(w.tanY > 0 ? w.tanY : 0.5f, 0.2f, 1.6f);
	c.encTanY = tanY * kOverscanY;
	c.encTanX = c.encTanY * c.eyeWidth / c.eyeHeight; // square pixels
	CopyName(c.pcName, sizeof(c.pcName), m_pcName);

	const Vr3dsConfig& o = m_s.config;
	bool changed = o.configId == 0 || o.codec != c.codec || o.stereo != c.stereo || o.eyeWidth != c.eyeWidth ||
				   o.eyeHeight != c.eyeHeight || o.fps != c.fps || o.bitrateKbps != c.bitrateKbps ||
				   std::fabs(o.encTanX - c.encTanX) > 1e-4f || std::fabs(o.encTanY - c.encTanY) > 1e-4f;
	m_viewVersion++;
	if (!changed) return;
	c.configId = m_nextConfigId++;
	if (m_nextConfigId == 0) m_nextConfigId = 1;
	m_s.config = c;
	m_keyRequests++;
	SendConfigLocked();
	Log("Stream: %s %s %ux%u per eye, %u fps, %u kbps, FOV %.0f x %.0f deg (with margin)",
		c.codec == VR3DS_CODEC_H264 ? "H.264" : "JPEG", c.stereo ? "3D" : "2D", c.eyeWidth, c.eyeHeight, c.fps,
		c.bitrateKbps, 2 * std::atan(c.encTanX) * 57.2958f, 2 * std::atan(c.encTanY) * 57.2958f);
}

void Streamer::StartSession(const Vr3dsHello& h, const sockaddr_in& from) {
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_s = Session();
		m_s.active = true;
		m_s.consoleId = h.consoleId;
		m_s.token = h.token;
		m_s.addr = from;
		m_s.name = FixedName(h.name, sizeof(h.name));
		m_s.model = h.model < VR3DS_MODEL_COUNT ? h.model : VR3DS_MODEL_3DS;
		m_s.caps = h.caps;
		m_s.sessionId = Random32() | 1;
		m_s.wanted = h.settings;
		m_s.lastRxUs = NowUs();
		if (m_pending.active && m_pending.hello.consoleId == h.consoleId) m_pending.active = false;
		m_poses.clear();
		BuildConfigLocked();
	}
	Log("Session started with \"%s\" (model %u, %s)", FixedName(h.name, sizeof(h.name)).c_str(), h.model,
		AddrStr(from).c_str());
	m_padControls = h.settings.controls;
	if (m_padControls != VR3DS_CONTROLS_VR) m_gamepad.Connect();
	m_audio.Start([this](const uint8_t* p, int n) {
		sockaddr_in to;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (!m_s.active) return;
			to = m_s.addr;
		}
		SendTo(p, n, to);
	});
	SendStatus();
}

void Streamer::EndSession(const char* reason) {
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (!m_s.active) return;
		Vr3dsHeader bye;
		vr3ds_header(&bye, VR3DS_PKT_BYE);
		for (int i = 0; i < 3; i++) SendTo(&bye, sizeof(bye), m_s.addr); // datagrams get lost
		m_s.active = false;
		m_viewVersion++;
	}
	Log("Session ended: %s", reason);
	m_audio.Stop();
	m_gamepad.Disconnect();
	m_dashboardOpen = false;
	if (m_onPad) m_onPad(Vr3dsGamepad{}, false, false, m_padControls);
}

void Streamer::OnHello(const Vr3dsHello& h, const sockaddr_in& from) {
	uint64_t now = NowUs();
	bool paired = m_pairing.IsPaired(h.consoleId, h.token);
	bool endOld = false;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_s.active) {
			if (m_s.consoleId != h.consoleId) return; // one console at a time
			if (paired) {
				// the console is searching again (it lost us): send it the stream again
				m_s.addr = from;
				m_s.lastRxUs = now;
				SendConfigLocked();
				return;
			}
			endOld = true; // it reset its pairing ("Forget PCs"): stop, then treat it as new
		}
	}
	if (endOld) EndSession("the console forgot this PC");
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (!paired) {
			auto rej = m_rejectedUntil.find(h.consoleId);
			if (rej != m_rejectedUntil.end() && rej->second > now) {
				SendPairStatus(from, VR3DS_PAIR_REJECTED, 0);
				return;
			}
			if (!m_pending.active || m_pending.hello.consoleId != h.consoleId) {
				m_pending.active = true;
				m_pending.code = (uint16_t)(Random32() % 10000);
				Log("Pairing request from \"%s\" (%s), code %04u", FixedName(h.name, sizeof(h.name)).c_str(),
					AddrStr(from).c_str(), m_pending.code);
			}
			m_pending.hello = h;
			m_pending.addr = from;
			m_pending.lastSeenUs = now;
			SendPairStatus(from, VR3DS_PAIR_PENDING, m_pending.code);
			return;
		}
	}
	StartSession(h, from);
}

void Streamer::OnPacket(const uint8_t* data, int len, const sockaddr_in& from) {
	if (!vr3ds_check(data, len, sizeof(Vr3dsHeader))) return;
	const Vr3dsHeader* hdr = (const Vr3dsHeader*)data;
	uint64_t now = NowUs();

	switch (hdr->type) {
	case VR3DS_PKT_HELLO:
		if (len >= (int)sizeof(Vr3dsHello)) {
			OnHello(*(const Vr3dsHello*)data, from);
			SendStatus();
		}
		break;

	case VR3DS_PKT_TRACKING: {
		if (len < (int)sizeof(Vr3dsTracking)) break;
		const Vr3dsTracking* t = (const Vr3dsTracking*)data;
		Quat q{t->qw, t->qx, t->qy, t->qz};
		float n = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
		if (!(n > 0.5f && n < 1.5f)) break;
		q = {q.w / n, q.x / n, q.y / n, q.z / n};
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (!m_s.active || !SameHost(from, m_s.addr)) break;
			m_s.lastRxUs = now;
			if (t->battery != m_s.battery || t->batteryFlags != m_s.batteryFlags || t->stereoStrength != m_s.stereoStrength)
				m_viewVersion++;
			m_s.battery = t->battery;
			m_s.batteryFlags = t->batteryFlags;
			m_s.stereoStrength = t->stereoStrength;
			if (t->shownFrameTs && t->shownFrameTs + t->shownFrameAgeUs <= now) {
				float ms = (now - t->shownFrameTs - t->shownFrameAgeUs) / 1000.0f;
				m_s.latencyMs = m_s.latencyMs > 0 ? m_s.latencyMs * 0.9f + ms * 0.1f : ms;
			}
			m_s.framesDecoded = t->framesDecoded;
			m_s.framesDropped = t->framesDropped;
			m_poses.push_back({now, q});
			if (m_poses.size() > 128) m_poses.pop_front();
		}
		if (m_onPose) m_onPose(q);
		{
			// Guide toggles the dashboard. The dashboard events confirm it,
			// this just does not wait for them.
			bool guide = (t->pad.buttons & VR3DS_BTN_GUIDE) != 0;
			if (guide && !m_guideDown) m_dashboardOpen = !m_dashboardOpen;
			m_guideDown = guide;
		}
		uint8_t controls;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			controls = m_s.wanted.controls;
		}
		if (controls != m_padControls) {
			// VR controllers replace the Xbox controller (games would get every button twice)
			m_padControls = controls;
			if (controls == VR3DS_CONTROLS_VR) m_gamepad.Disconnect();
			else m_gamepad.Connect();
			SendStatus();
		}
		if (m_onPad) m_onPad(t->pad, true, m_dashboardOpen, controls);
		{
			// Guide opens SteamVR's dashboard (on Windows the Xbox button opens the Game Bar)
			Vr3dsGamepad xpad = t->pad;
			if (m_dashboardOpen) memset(&xpad, 0, sizeof(xpad));
			xpad.buttons &= (uint16_t)~VR3DS_BTN_GUIDE;
			m_gamepad.Update(xpad);
		}
		break;
	}

	case VR3DS_PKT_SETTINGS: {
		if (len < (int)sizeof(Vr3dsSettingsPacket)) break;
		const Vr3dsSettingsPacket* p = (const Vr3dsSettingsPacket*)data;
		std::lock_guard<std::mutex> lock(m_mutex);
		if (!m_s.active || !SameHost(from, m_s.addr)) break;
		m_s.lastRxUs = now;
		if (memcmp(&p->settings, &m_s.wanted, sizeof(p->settings)) != 0) {
			m_s.wanted = p->settings;
			m_s.wantedChangedUs = now; // applied once the user stops changing them
		}
		break;
	}

	case VR3DS_PKT_KEYFRAME_REQ: {
		if (len < (int)sizeof(Vr3dsKeyframeRequest)) break;
		const Vr3dsKeyframeRequest* r = (const Vr3dsKeyframeRequest*)data;
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_s.active && SameHost(from, m_s.addr) && r->configId == m_s.config.configId) m_keyRequests++;
		break;
	}

	case VR3DS_PKT_BYE: {
		bool ours;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			ours = m_s.active && SameHost(from, m_s.addr);
		}
		if (ours) EndSession("the console left");
		break;
	}
	default:
		break;
	}
}

void Streamer::OnIpc(const uint8_t* data, int len) {
	if (len < (int)sizeof(IpcCommand) || ((const IpcCommand*)data)->magic != VR3DS_IPC_MAGIC) return;
	uint8_t type = ((const IpcCommand*)data)->type;
	if (type == IPC_PAIR_DECISION && len >= (int)sizeof(IpcPairDecision)) {
		const IpcPairDecision* d = (const IpcPairDecision*)data;
		Vr3dsHello hello;
		sockaddr_in addr;
		bool start = false;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (!m_pending.active || m_pending.hello.consoleId != d->consoleId) return;
			hello = m_pending.hello;
			addr = m_pending.addr;
			m_pending.active = false;
			if (d->accept) {
				m_pairing.Add(hello.consoleId, hello.token, FixedName(hello.name, sizeof(hello.name)));
				start = !m_s.active;
				Log("Paired with \"%s\"", FixedName(hello.name, sizeof(hello.name)).c_str());
			} else {
				m_rejectedUntil[hello.consoleId] = NowUs() + kRejectUs;
				SendPairStatus(addr, VR3DS_PAIR_REJECTED, 0);
				Log("Pairing refused for \"%s\"", FixedName(hello.name, sizeof(hello.name)).c_str());
			}
		}
		if (start) StartSession(hello, addr);
		SendStatus();
	} else if (type == IPC_FORGET_ALL) {
		m_pairing.Clear();
		Log("All pairings forgotten");
		EndSession("pairings cleared");
		SendStatus();
	}
}

void Streamer::SendStatus() {
	if (m_ipcSock == INVALID_SOCKET) return;
	IpcStatus st = {};
	st.magic = VR3DS_IPC_MAGIC;
	st.type = IPC_STATUS;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		st.state = m_s.active ? IPC_STATE_STREAMING : IPC_STATE_WAITING;
		if (m_s.active) {
			st.model = m_s.model;
			st.codec = m_s.config.codec;
			st.stereo = m_s.config.stereo;
			st.battery = m_s.battery;
			st.batteryFlags = m_s.batteryFlags;
			st.eyeWidth = m_s.config.eyeWidth;
			st.eyeHeight = m_s.config.eyeHeight;
			st.fps = m_s.config.fps;
			st.bitrateKbps = m_s.config.bitrateKbps;
			st.framesDropped = m_s.framesDropped;
			CopyName(st.consoleName, sizeof(st.consoleName), m_s.name);
			CopyName(st.consoleIp, sizeof(st.consoleIp), AddrStr(m_s.addr));
			st.latencyMs = m_s.latencyMs;
		}
		st.pairPending = m_pending.active ? 1 : 0;
		if (m_pending.active) {
			st.pairCode = m_pending.code;
			st.pairConsoleId = m_pending.hello.consoleId;
			st.pairModel = m_pending.hello.model;
			CopyName(st.pairName, sizeof(st.pairName), FixedName(m_pending.hello.name, sizeof(m_pending.hello.name)));
			CopyName(st.pairIp, sizeof(st.pairIp), AddrStr(m_pending.addr));
		}
		st.encodeFps = m_encodeFps;
		st.sentMbps = m_sentMbps;
		CopyName(st.encoder, sizeof(st.encoder), m_encoderName);
	}
	st.gamepad = m_padControls == VR3DS_CONTROLS_VR ? IPC_PAD_VR : m_gamepad.Status();
	st.audio = m_audio.Running() ? 1 : 0;
	st.pairedCount = (uint32_t)m_pairing.Count();

	sockaddr_in to = {};
	to.sin_family = AF_INET;
	to.sin_port = htons(VR3DS_IPC_APP_PORT);
	inet_pton(AF_INET, "127.0.0.1", &to.sin_addr);
	sendto(m_ipcSock, (const char*)&st, sizeof(st), 0, (const sockaddr*)&to, sizeof(to));
}

void Streamer::NetThread() {
	uint8_t buf[2048];
	uint64_t lastStatus = 0;
	while (m_running) {
		fd_set rs;
		FD_ZERO(&rs);
		FD_SET(m_sock, &rs);
		if (m_ipcSock != INVALID_SOCKET) FD_SET(m_ipcSock, &rs);
		timeval tv = {0, 10000};
		if (select(0, &rs, nullptr, nullptr, &tv) > 0) {
			if (FD_ISSET(m_sock, &rs)) {
				for (int i = 0; i < 256; i++) {
					sockaddr_in from;
					int fl = sizeof(from);
					int n = recvfrom(m_sock, (char*)buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
					if (n <= 0) break;
					OnPacket(buf, n, from);
				}
			}
			if (m_ipcSock != INVALID_SOCKET && FD_ISSET(m_ipcSock, &rs)) {
				int n = recv(m_ipcSock, (char*)buf, sizeof(buf), 0);
				if (n > 0) OnIpc(buf, n);
			}
		}

		uint64_t now = NowUs();
		const char* endReason = nullptr;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (m_s.active) {
				if (now - m_s.lastRxUs > kSessionTimeoutUs) {
					endReason = "the console stopped answering";
				} else {
					if (m_s.wantedChangedUs && now - m_s.wantedChangedUs > kSettingsDebounceUs) {
						m_s.wantedChangedUs = 0;
						BuildConfigLocked();
					}
					if (now - m_s.lastConfigSentUs > kConfigResendUs) SendConfigLocked();
				}
			}
			if (m_pending.active && now - m_pending.lastSeenUs > kPendingTimeoutUs) m_pending.active = false;
			if (now - m_statWindowUs >= 1000000) {
				float dt = (now - m_statWindowUs) / 1e6f;
				m_encodeFps = m_statFrames.exchange(0) / dt;
				m_sentMbps = m_statBytes.exchange(0) * 8.0f / dt / 1e6f;
				m_statWindowUs = now;
			}
		}
		if (endReason) EndSession(endReason);
		if (now - lastStatus > kStatusPeriodUs) {
			lastStatus = now;
			SendStatus();
		}
	}
}

// ----------------------------------------------------------------- video

Quat Streamer::ResolveFramePose(const Quat& rendered, uint64_t presentUs) {
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_poses.empty()) return rendered;
	// The layer pose is the pose we reported, read back by SteamVR; find it in
	// our history so the console gets exactly the quaternion it sent.
	const PoseSample* best = nullptr;
	float bestErr = 1e9f;
	for (const PoseSample& p : m_poses) {
		float dot = std::fabs(p.q.w * rendered.w + p.q.x * rendered.x + p.q.y * rendered.y + p.q.z * rendered.z);
		float err = 1.0f - dot;
		if (err < bestErr) {
			bestErr = err;
			best = &p;
		}
	}
	if (best && bestErr < 2e-4f) return best->q;
	// Not in our space (e.g. a universe offset): use what we reported about one frame earlier.
	static bool warned = false;
	if (!warned) {
		warned = true;
		Log("Layer poses are not in driver space; using the pose history instead");
	}
	uint64_t target = presentUs > 20000 ? presentUs - 20000 : 0;
	const PoseSample* close = &m_poses.back();
	for (const PoseSample& p : m_poses)
		if (p.timeUs <= target) close = &p;
	return close->q;
}

void Streamer::SendFrame(const EncodedFrame& f, const FrameSlot& slot, uint16_t configId, uint32_t frameId,
						 const sockaddr_in& to, int kbps) {
	uint8_t pkt[VR3DS_MAX_DATAGRAM];
	Vr3dsVideoHeader* h = (Vr3dsVideoHeader*)pkt;
	vr3ds_header(&h->h, VR3DS_PKT_VIDEO);
	h->frameId = frameId;
	h->configId = configId;
	h->flags = f.key ? VR3DS_FRAME_KEY : 0;
	h->reserved = 0;
	h->frameSize = (uint32_t)f.data.size();
	h->timestampUs = slot.timestampUs;
	h->qw = slot.pose.w;
	h->qx = slot.pose.x;
	h->qy = slot.pose.y;
	h->qz = slot.pose.z;
	uint32_t total = (uint32_t)f.data.size();
	uint16_t count = (uint16_t)((total + VR3DS_VIDEO_PAYLOAD - 1) / VR3DS_VIDEO_PAYLOAD);
	h->fragCount = count;

	// Pace the fragments: a burst would overflow the console's small Wi-Fi buffers.
	double paceKbps = std::clamp(kbps * 4.0, 10000.0, 24000.0);
	double intervalUs = VR3DS_MAX_DATAGRAM * 8.0 * 1000.0 / paceKbps;
	uint64_t start = NowUs();
	for (uint16_t i = 0; i < count; i++) {
		uint64_t target = start + (uint64_t)(i * intervalUs);
		for (uint64_t now = NowUs(); now < target; now = NowUs()) {
			if (target - now > 1500) Sleep(1);
			else SwitchToThread();
		}
		uint32_t off = (uint32_t)i * VR3DS_VIDEO_PAYLOAD;
		uint32_t n = std::min<uint32_t>(VR3DS_VIDEO_PAYLOAD, total - off);
		h->fragIndex = i;
		memcpy(pkt + sizeof(Vr3dsVideoHeader), f.data.data() + off, n);
		int len = (int)(sizeof(Vr3dsVideoHeader) + n);
		SendTo(pkt, len, to);
		m_statBytes += (uint64_t)len + 28;
	}
	m_statFrames++;
}

void Streamer::EncodeThread() {
	CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
	uint32_t frameId = 0;
	uint16_t lastConfig = 0;
	uint32_t keysServed = 0;
	uint64_t lastSentUs = 0;

	while (m_running) {
		FrameSlot slot;
		{
			std::unique_lock<std::mutex> lock(m_frameMutex);
			m_frameCv.wait_for(lock, std::chrono::milliseconds(100), [&] { return m_hasQueued || !m_running; });
			if (!m_hasQueued) continue;
			slot = m_queued;
			m_hasQueued = false;
		}

		Vr3dsConfig cfg;
		sockaddr_in to;
		bool active;
		uint32_t keyRequests;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			active = m_s.active;
			cfg = m_s.config;
			to = m_s.addr;
			keyRequests = m_keyRequests;
		}
		if (!active || cfg.configId == 0 || slot.format.configId != cfg.configId) {
			m_compositor->Release(slot);
			continue;
		}

		// SteamVR runs at 60 Hz; send at the console's frame rate
		uint64_t now = NowUs();
		uint64_t interval = 1000000ull / std::max<int>(cfg.fps, 1);
		if (lastSentUs && now - lastSentUs + 4000 < interval) {
			m_compositor->Release(slot);
			continue;
		}

		int h = vr3ds_picture_height(cfg.codec, cfg.stereo, cfg.eyeHeight);
		bool reinit = !m_encoder || m_encCodec != cfg.codec || m_encW != cfg.eyeWidth || m_encH != h ||
					  m_encFps != cfg.fps || m_encKbps != cfg.bitrateKbps;
		if (reinit) {
			m_encoder = cfg.codec == VR3DS_CODEC_H264 ? CreateH264Encoder() : CreateJpegEncoder();
			if (!m_encoder->Init(cfg.eyeWidth, h, cfg.fps, cfg.bitrateKbps)) {
				Log("Encoder initialisation failed");
				m_encoder.reset();
				m_compositor->Release(slot);
				Sleep(500);
				continue;
			}
			m_encCodec = cfg.codec;
			m_encW = cfg.eyeWidth;
			m_encH = (uint16_t)h;
			m_encFps = cfg.fps;
			m_encKbps = cfg.bitrateKbps;
			std::lock_guard<std::mutex> lock(m_mutex);
			m_encoderName = m_encoder->Name();
			keysServed = keyRequests - 1; // a new encoder starts with a key frame
		}
		if (cfg.configId != lastConfig) {
			lastConfig = cfg.configId;
			frameId = 0;
			keysServed = keyRequests - 1; // every config starts with a key frame
		}
		bool forceKey = keysServed != keyRequests;

		const uint8_t* px = nullptr;
		uint32_t pitch = 0;
		if (!m_compositor->Map(slot, &px, &pitch)) {
			m_compositor->Release(slot);
			continue;
		}
		EncodedFrame ef;
		bool ok = m_encoder->Encode(px, pitch, forceKey, ef);
		m_compositor->Release(slot);

		// every encoded frame takes an id, sent or not, so the console sees gaps
		uint32_t id = frameId++;
		if (!ok || ef.data.size() > VR3DS_MAX_FRAME) {
			keysServed = keyRequests - 1; // the console will miss a reference: next one is a key frame
			continue;
		}
		if (ef.key) keysServed = keyRequests;
		slot.pose = ResolveFramePose(slot.pose, slot.timestampUs);
		SendFrame(ef, slot, cfg.configId, id, to, cfg.bitrateKbps);
		lastSentUs = now;
	}
	m_encoder.reset();
	CoUninitialize();
}
