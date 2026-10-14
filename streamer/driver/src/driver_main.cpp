// SteamVR entry point of the 3DS VR driver.
#include <windows.h>
#include <timeapi.h>

#include <cstring>
#include <memory>

#include <openvr_driver.h>

#include "compositor.h"
#include "controls.h"
#include "hmd.h"
#include "ipc.h"
#include "log.h"
#include "streamer.h"

using namespace vr;

class ServerProvider : public IServerTrackedDeviceProvider {
public:
	EVRInitError Init(IVRDriverContext* ctx) override {
		VR_INIT_SERVER_DRIVER_CONTEXT(ctx);
		LogInit();

		// Only active while the 3DS VR app runs, so that other headsets work
		// normally the rest of the time.
		HANDLE app = OpenMutexW(SYNCHRONIZE, FALSE, VR3DS_APP_MUTEX);
		if (!app) {
			Log("The 3DS VR app is not running: driver inactive");
			return VRInitError_Init_HmdNotFound;
		}
		CloseHandle(app);

		timeBeginPeriod(1);
		m_timerPeriod = true;
		m_compositor = std::make_unique<Compositor>();
		if (!m_compositor->Init()) return Fail("No usable Direct3D 11 GPU");
		m_streamer = std::make_unique<Streamer>();
		m_hmd = std::make_unique<Hmd>(m_compositor.get(), m_streamer.get());
		m_controls = std::make_unique<Controls>();
		Hmd* hmd = m_hmd.get();
		Controls* controls = m_controls.get();
		if (!m_streamer->Start(
				m_compositor.get(), [hmd](const Quat& q) { hmd->OnTracking(q); },
				[hmd, controls](const Vr3dsGamepad& pad, bool connected, bool dashboard) {
					hmd->SetSystemButton(connected && (pad.buttons & VR3DS_BTN_GUIDE));
					// only there while the dashboard is open: games (VRChat) would see a tracker
					// and get every button twice, from it and from the Xbox controller
					controls->Update(pad, connected && dashboard);
				}))
			return Fail("Network unavailable");
		if (!VRServerDriverHost()->TrackedDeviceAdded("3DSVR-HMD", TrackedDeviceClass_HMD, m_hmd.get()))
			return Fail("SteamVR refused the headset");
		if (!VRServerDriverHost()->TrackedDeviceAdded("3DSVR-CONTROLS", TrackedDeviceClass_Controller, controls))
			Log("SteamVR refused the console controls");
		Log("3DS VR driver ready");
		return VRInitError_None;
	}

	void Cleanup() override {
		Teardown();
		VR_CLEANUP_SERVER_DRIVER_CONTEXT();
	}

	const char* const* GetInterfaceVersions() override { return k_InterfaceVersions; }
	void RunFrame() override {
		VREvent_t e;
		while (VRServerDriverHost()->PollNextEvent(&e, sizeof(e))) {
			if (!m_streamer) continue;
			if (e.eventType == VREvent_DashboardActivated || e.eventType == VREvent_DashboardDeactivated) {
				bool open = e.eventType == VREvent_DashboardActivated;
				Log("SteamVR dashboard %s", open ? "opened" : "closed");
				m_streamer->SetDashboardOpen(open);
			}
		}
	}
	bool ShouldBlockStandbyMode() override { return false; }
	void EnterStandby() override {}
	void LeaveStandby() override {}

private:
	// Init failures are not followed by Cleanup(): stop everything here.
	EVRInitError Fail(const char* why) {
		Log("%s", why);
		Teardown();
		return VRInitError_Driver_Failed;
	}

	void Teardown() {
		if (m_streamer) m_streamer->Stop();
		if (m_hmd) m_hmd->Stop();
		if (m_compositor) m_compositor->Shutdown();
		// The objects stay allocated: SteamVR may still call into the headset
		// (and through it the compositor and streamer) while it shuts down.
		m_streamer.release();
		m_compositor.release();
		m_hmd.release();
		m_controls.release();
		if (m_timerPeriod) timeEndPeriod(1);
		m_timerPeriod = false;
	}

	bool m_timerPeriod = false;
	std::unique_ptr<Compositor> m_compositor;
	std::unique_ptr<Streamer> m_streamer;
	std::unique_ptr<Hmd> m_hmd;
	std::unique_ptr<Controls> m_controls;
};

static ServerProvider g_provider;

extern "C" __declspec(dllexport) void* HmdDriverFactory(const char* interfaceName, int* returnCode) {
	if (!strcmp(interfaceName, IServerTrackedDeviceProvider_Version)) return &g_provider;
	if (returnCode) *returnCode = VRInitError_Init_InterfaceNotFound;
	return nullptr;
}
