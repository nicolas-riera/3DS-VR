#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

#include <openvr_driver.h>

#include "compositor.h"
#include "math_util.h"
#include "streamer.h"

// The headset SteamVR sees. It takes its orientation from the console, its
// model, icons and battery from the session, and receives every rendered frame
// through the direct mode component.
class Hmd : public vr::ITrackedDeviceServerDriver,
			public vr::IVRDisplayComponent,
			public vr::IVRDriverDirectModeComponent {
public:
	Hmd(Compositor* compositor, Streamer* streamer) : m_compositor(compositor), m_streamer(streamer) {}

	void OnTracking(const Quat& q);
	// The headset's system button (Guide on the console): toggles SteamVR's dashboard.
	void SetSystemButton(bool down);
	void Stop();

	// ITrackedDeviceServerDriver
	vr::EVRInitError Activate(uint32_t unObjectId) override;
	void Deactivate() override;
	void EnterStandby() override {}
	void* GetComponent(const char* pchComponentNameAndVersion) override;
	void DebugRequest(const char*, char* pchResponseBuffer, uint32_t unResponseBufferSize) override;
	vr::DriverPose_t GetPose() override;

	// IVRDisplayComponent
	void GetWindowBounds(int32_t* pnX, int32_t* pnY, uint32_t* pnWidth, uint32_t* pnHeight) override;
	bool IsDisplayOnDesktop() override { return false; }
	bool IsDisplayRealDisplay() override { return false; }
	void GetRecommendedRenderTargetSize(uint32_t* pnWidth, uint32_t* pnHeight) override;
	void GetEyeOutputViewport(vr::EVREye eEye, uint32_t* pnX, uint32_t* pnY, uint32_t* pnWidth,
							  uint32_t* pnHeight) override;
	void GetProjectionRaw(vr::EVREye eEye, float* pfLeft, float* pfRight, float* pfTop, float* pfBottom) override;
	vr::DistortionCoordinates_t ComputeDistortion(vr::EVREye eEye, float fU, float fV) override;
	bool ComputeInverseDistortion(vr::HmdVector2_t* pResult, vr::EVREye, uint32_t, float fU, float fV) override;

	// IVRDriverDirectModeComponent
	void CreateSwapTextureSet(uint32_t unPid, const SwapTextureSetDesc_t* pSwapTextureSetDesc,
							  SwapTextureSet_t* pOutSwapTextureSet) override;
	void DestroySwapTextureSet(vr::SharedTextureHandle_t sharedTextureHandle) override;
	void DestroyAllSwapTextureSets(uint32_t unPid) override;
	void GetNextSwapTextureSetIndex(vr::SharedTextureHandle_t sharedTextureHandles[2], uint32_t (*pIndices)[2]) override;
	void SubmitLayer(const SubmitLayerPerEye_t (&perEye)[2]) override;
	void Present(vr::SharedTextureHandle_t syncTexture) override;
	void PostPresent(const Throttling_t* pThrottling) override;

private:
	void VsyncThread();
	void ApplyView(const SessionView& v);
	void UpdatePose(const Quat& q, bool connected);
	void SetModelProperties(uint8_t model);

	Compositor* m_compositor;
	Streamer* m_streamer;
	std::atomic<uint32_t> m_id{vr::k_unTrackedDeviceIndexInvalid};
	vr::PropertyContainerHandle_t m_props = vr::k_ulInvalidPropertyContainer;
	vr::VRInputComponentHandle_t m_systemButton = vr::k_ulInvalidInputComponentHandle;

	std::mutex m_poseMutex;
	vr::DriverPose_t m_pose{};
	Quat m_lastQ;
	float m_eyeHeight = 1.59f;

	std::atomic<float> m_tanX{1.158f}, m_tanY{0.579f};
	SessionView m_applied;
	bool m_appliedOnce = false;
	uint8_t m_appliedModel = 0xFF;
	float m_appliedIpd = -1.0f;
	uint8_t m_appliedBattery = 0xFF, m_appliedBatteryFlags = 0xFF;
	bool m_appliedConnected = false;

	std::thread m_vsyncThread;
	std::atomic<bool> m_running{false};
	std::mutex m_vsyncMutex;
	std::condition_variable m_vsyncCv;
	uint64_t m_vsyncCount = 0;
};
