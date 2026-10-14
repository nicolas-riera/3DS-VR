// SteamVR entry point of the 3DS VR driver.
#include <windows.h>
#include <timeapi.h>

#include <cstring>
#include <memory>

#include <openvr_driver.h>

#include "compositor.h"
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
		m_compositor = std::make_unique<Compositor>();
		if (!m_compositor->Init()) {
			Log("No usable Direct3D 11 GPU");
			return VRInitError_Driver_Failed;
		}
		m_streamer = std::make_unique<Streamer>();
		m_hmd = std::make_unique<Hmd>(m_compositor.get(), m_streamer.get());
		Hmd* hmd = m_hmd.get();
		if (!m_streamer->Start(m_compositor.get(), [hmd](const Quat& q) { hmd->OnTracking(q); })) {
			Log("Network unavailable");
			return VRInitError_Driver_Failed;
		}
		if (!VRServerDriverHost()->TrackedDeviceAdded("3DSVR-HMD", TrackedDeviceClass_HMD, m_hmd.get())) {
			Log("SteamVR refused the headset");
			return VRInitError_Driver_Failed;
		}
		Log("3DS VR driver ready");
		return VRInitError_None;
	}

	void Cleanup() override {
		if (m_streamer) m_streamer->Stop();
		if (m_hmd) m_hmd->Stop();
		if (m_compositor) m_compositor->Shutdown();
		m_streamer.reset();
		m_compositor.reset();
		// the Hmd object stays alive: SteamVR may still hold its pointer
		if (m_hmd) m_hmd.release();
		timeEndPeriod(1);
		VR_CLEANUP_SERVER_DRIVER_CONTEXT();
	}

	const char* const* GetInterfaceVersions() override { return k_InterfaceVersions; }
	void RunFrame() override {}
	bool ShouldBlockStandbyMode() override { return false; }
	void EnterStandby() override {}
	void LeaveStandby() override {}

private:
	std::unique_ptr<Compositor> m_compositor;
	std::unique_ptr<Streamer> m_streamer;
	std::unique_ptr<Hmd> m_hmd;
};

static ServerProvider g_provider;

extern "C" __declspec(dllexport) void* HmdDriverFactory(const char* interfaceName, int* returnCode) {
	if (!strcmp(interfaceName, IServerTrackedDeviceProvider_Version)) return &g_provider;
	if (returnCode) *returnCode = VRInitError_Init_InterfaceNotFound;
	return nullptr;
}
