#pragma once
#include <winsock2.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "audio_capture.h"
#include "compositor.h"
#include "encoder.h"
#include "gamepad.h"
#include "ipc.h"
#include "math_util.h"
#include "pairing.h"
#include "vr3ds_protocol.h"

// What the headset device needs to know about the current session.
struct SessionView {
	bool connected = false;
	uint8_t model = VR3DS_MODEL_COUNT; // VR3DS_MODEL_COUNT: unknown
	uint8_t battery = 100;
	uint8_t batteryFlags = 0;
	float encTanX = 1.16f, encTanY = 0.58f; // projection of the rendered eye images
	float ipd = 0.063f;                      // 0 for a mono stream
	float eyeHeight = 1.59f;                 // metres above the floor, from the player's height
	uint32_t version = 0;                    // changes whenever anything above changes
};

// Network side of the driver: discovery, pairing, session, video encoding
// and sending, audio, gamepad, and the local link to the 3DS VR app.
class Streamer {
public:
	using PoseCallback = std::function<void(const Quat& q)>;
	// The console's buttons for SteamVR; connected = false when the session
	// ends. dashboard: SteamVR's dashboard is open, so the buttons drive it.
	// controls: Vr3dsControls, what the console wants its buttons to be.
	using PadCallback =
		std::function<void(const Vr3dsGamepad& pad, bool connected, bool dashboard, uint8_t controls)>;

	bool Start(Compositor* compositor, PoseCallback onPose, PadCallback onPad);
	void Stop();
	// From SteamVR's dashboard events. While it is open the buttons drive it, not the game.
	void SetDashboardOpen(bool open) { m_dashboardOpen = open; }

	SessionView View();
	OutputFormat Format();
	// A composited picture is ready (from SteamVR's present thread).
	void OnFrame(const FrameSlot& slot);

private:
	struct Session {
		bool active = false;
		uint64_t consoleId = 0, token = 0;
		sockaddr_in addr{};
		std::string name;
		uint8_t model = 0, caps = 0;
		uint32_t sessionId = 0;
		Vr3dsStreamSettings wanted{};
		uint64_t wantedChangedUs = 0; // FOV changes are applied once the user stops changing them
		Vr3dsConfig config{};
		uint8_t stereoStrength = 255;
		uint8_t battery = 100, batteryFlags = 0;
		uint64_t lastRxUs = 0, lastConfigSentUs = 0;
		// stats echoed by the console
		float latencyMs = 0; // PC capture -> console reception, plus the trip back of the TRACKING packet
		uint32_t framesDecoded = 0, framesDropped = 0;
	};
	struct PendingPair {
		bool active = false;
		Vr3dsHello hello{};
		sockaddr_in addr{};
		uint16_t code = 0;
		uint64_t lastSeenUs = 0;
	};
	struct PoseSample {
		uint64_t timeUs;
		Quat q;
	};

	void NetThread();
	void EncodeThread();
	void OnPacket(const uint8_t* data, int len, const sockaddr_in& from);
	void OnIpc(const uint8_t* data, int len);
	void OnHello(const Vr3dsHello& h, const sockaddr_in& from);
	void StartSession(const Vr3dsHello& h, const sockaddr_in& from);
	void EndSession(const char* reason);
	void BuildConfigLocked();
	void SendConfigLocked();
	void SendPairStatus(const sockaddr_in& to, uint8_t state, uint16_t code);
	void SendStatus();
	void SendTo(const void* data, int len, const sockaddr_in& to);
	Quat ResolveFramePose(const Quat& rendered, uint64_t presentUs);
	void SendFrame(const EncodedFrame& f, const FrameSlot& slot, uint16_t configId, uint32_t frameId,
				   const sockaddr_in& to, int kbps);

	Compositor* m_compositor = nullptr;
	PoseCallback m_onPose;
	PadCallback m_onPad;
	std::atomic<bool> m_dashboardOpen{false};
	bool m_guideDown = false; // network thread
	std::atomic<uint8_t> m_padControls{VR3DS_CONTROLS_XBOX}; // the Xbox controller is plugged in for XBOX
	std::atomic<bool> m_running{false};
	std::thread m_netThread, m_encodeThread;
	SOCKET m_sock = INVALID_SOCKET;
	SOCKET m_ipcSock = INVALID_SOCKET;
	std::string m_pcName;

	std::mutex m_mutex; // everything below
	Session m_s;
	PendingPair m_pending;
	std::map<uint64_t, uint64_t> m_rejectedUntil;
	uint16_t m_nextConfigId = 1;
	uint32_t m_viewVersion = 1;
	uint32_t m_keyRequests = 1; // incremented on each key frame request; the encoder serves them in order
	std::deque<PoseSample> m_poses;
	PairingStore m_pairing;

	// encoder thread
	std::mutex m_frameMutex;
	std::condition_variable m_frameCv;
	FrameSlot m_queued;
	bool m_hasQueued = false;
	std::unique_ptr<VideoEncoder> m_encoder;
	uint8_t m_encCodec = 0;
	uint16_t m_encW = 0, m_encH = 0, m_encFps = 0, m_encKbps = 0;
	std::string m_encoderName;

	Gamepad m_gamepad;
	AudioCapture m_audio;

	// stats (encode thread writes, net thread reads)
	std::atomic<uint32_t> m_statFrames{0};
	std::atomic<uint64_t> m_statBytes{0};
	float m_encodeFps = 0, m_sentMbps = 0;
	uint64_t m_statWindowUs = 0;
};
