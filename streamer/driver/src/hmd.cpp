#include "hmd.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "log.h"

using namespace vr;

static constexpr float kRefreshHz = 60.0f;
static constexpr uint32_t kEyeTargetW = 1024, kEyeTargetH = 512; // 2x the H.264 eye size: supersampled
// neck model: the eyes turn around a pivot below and behind them
static constexpr float kNeckUp = 0.11f, kNeckForward = 0.08f;

static const char* const kModelSlugs[VR3DS_MODEL_COUNT] = {"3ds", "3dsxl", "new3ds", "2ds", "new3dsxl", "new2dsxl"};
static const char* const kModelNames[VR3DS_MODEL_COUNT] = {
	"Nintendo 3DS", "Nintendo 3DS XL", "New Nintendo 3DS", "Nintendo 2DS", "New Nintendo 3DS XL", "New Nintendo 2DS XL"};

// Our own tracking universe, "3DSVR" in ASCII.
static constexpr uint64_t kUniverseId = 0x3344535652ull;

// The universe's play area, given to SteamVR so that it never asks for Room
// Setup: there is no room to set up with a 3DoF headset. 2 x 2 m, hidden.
static std::string ChaperoneJson(float eyeHeight) {
	static const float c[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
	std::string walls;
	char buf[256];
	for (int i = 0; i < 4; i++) {
		const float *a = c[i], *b = c[(i + 1) % 4];
		snprintf(buf, sizeof(buf), "%s[[%g,0,%g],[%g,0,%g],[%g,2.5,%g],[%g,2.5,%g]]", i ? "," : "", a[0], a[1], b[0],
				 b[1], b[0], b[1], a[0], a[1]);
		walls += buf;
	}
	snprintf(buf, sizeof(buf),
			 "],\"play_area\":[2,2],\"standing\":{\"translation\":[0,0,0],\"yaw\":0},"
			 "\"seated\":{\"translation\":[0,%.3f,0],\"yaw\":0}}]}",
			 eyeHeight);
	return "{\"jsonid\":\"chaperone_info\",\"version\":5,\"universes\":[{\"universeID\":\"" +
		   std::to_string(kUniverseId) + "\",\"collision_bounds\":[" + walls + buf;
}

static HmdMatrix34_t Translation(float x) {
	HmdMatrix34_t m = {};
	m.m[0][0] = m.m[1][1] = m.m[2][2] = 1.0f;
	m.m[0][3] = x;
	return m;
}

// ------------------------------------------------------------- lifecycle

EVRInitError Hmd::Activate(uint32_t unObjectId) {
	m_id = unObjectId;
	CVRPropertyHelpers* p = VRProperties();
	m_props = p->TrackedDeviceToPropertyContainer(unObjectId);

	p->SetStringProperty(m_props, Prop_TrackingSystemName_String, "3dsvr");
	p->SetStringProperty(m_props, Prop_SerialNumber_String, "3DSVR-HMD");
	p->SetStringProperty(m_props, Prop_DriverVersion_String, "1.0.0");
	p->SetStringProperty(m_props, Prop_InputProfilePath_String, "{3dsvr}/input/3dsvr_hmd_profile.json");
	SetModelProperties(VR3DS_MODEL_COUNT);

	p->SetFloatProperty(m_props, Prop_UserIpdMeters_Float, 0.063f);
	p->SetFloatProperty(m_props, Prop_UserHeadToEyeDepthMeters_Float, 0.0f);
	p->SetFloatProperty(m_props, Prop_DisplayFrequency_Float, kRefreshHz);
	p->SetFloatProperty(m_props, Prop_SecondsFromVsyncToPhotons_Float, 0.005f);
	p->SetBoolProperty(m_props, Prop_IsOnDesktop_Bool, false);
	p->SetBoolProperty(m_props, Prop_DisplayDebugMode_Bool, false);
	p->SetBoolProperty(m_props, Prop_HasDriverDirectModeComponent_Bool, true);
	p->SetBoolProperty(m_props, Prop_DriverDirectModeSendsVsyncEvents_Bool, true);
	p->SetUint64Property(m_props, Prop_GraphicsAdapterLuid_Uint64, m_compositor->AdapterLuid());
	p->SetBoolProperty(m_props, Prop_HasCamera_Bool, false);
	p->SetBoolProperty(m_props, Prop_ContainsProximitySensor_Bool, false);
	p->SetBoolProperty(m_props, Prop_DeviceCanPowerOff_Bool, false);
	p->SetBoolProperty(m_props, Prop_DeviceProvidesBatteryStatus_Bool, true);
	p->SetFloatProperty(m_props, Prop_DeviceBatteryPercentage_Float, 1.0f);
	p->SetBoolProperty(m_props, Prop_DeviceIsCharging_Bool, false);

	p->SetUint64Property(m_props, Prop_CurrentUniverseId_Uint64, kUniverseId);
	p->SetStringProperty(m_props, Prop_DriverProvidedChaperoneJson_String, ChaperoneJson(m_eyeHeight).c_str());
	p->SetBoolProperty(m_props, Prop_DriverProvidedChaperoneVisibility_Bool, false);
	VRDriverInput()->CreateBooleanComponent(m_props, "/input/system/click", &m_systemButton);

	{
		std::lock_guard<std::mutex> lock(m_poseMutex);
		m_pose = {};
		m_pose.qWorldFromDriverRotation.w = 1;
		m_pose.qDriverFromHeadRotation.w = 1;
		m_pose.qRotation.w = 1;
		m_pose.vecPosition[1] = m_eyeHeight;
		m_pose.result = TrackingResult_Uninitialized;
		m_pose.poseIsValid = false;
		m_pose.deviceIsConnected = false;
	}

	m_running = true;
	m_vsyncThread = std::thread(&Hmd::VsyncThread, this);
	Log("Headset activated (device %u)", unObjectId);
	return VRInitError_None;
}

void Hmd::Deactivate() {
	Stop();
	m_id = k_unTrackedDeviceIndexInvalid;
}

void Hmd::Stop() {
	if (!m_running) return;
	m_running = false;
	m_vsyncCv.notify_all();
	if (m_vsyncThread.joinable()) m_vsyncThread.join();
}

void* Hmd::GetComponent(const char* name) {
	if (!strcmp(name, IVRDisplayComponent_Version)) return static_cast<IVRDisplayComponent*>(this);
	if (!strcmp(name, IVRDriverDirectModeComponent_Version)) return static_cast<IVRDriverDirectModeComponent*>(this);
	return nullptr;
}

void Hmd::DebugRequest(const char*, char* buf, uint32_t size) {
	if (size) buf[0] = 0;
}

// ------------------------------------------------------------------ pose

DriverPose_t Hmd::GetPose() {
	std::lock_guard<std::mutex> lock(m_poseMutex);
	return m_pose;
}

void Hmd::UpdatePose(const Quat& q, bool connected) {
	uint32_t id = m_id;
	DriverPose_t pose;
	{
		std::lock_guard<std::mutex> lock(m_poseMutex);
		m_lastQ = q;
		// position from the neck model: looking straight ahead puts the eyes at m_eyeHeight
		float offset[3] = {0.0f, kNeckUp, -kNeckForward}, r[3];
		QuatRotate(q, offset, r);
		m_pose.vecPosition[0] = r[0];
		m_pose.vecPosition[1] = m_eyeHeight - kNeckUp + r[1];
		m_pose.vecPosition[2] = kNeckForward + r[2];
		m_pose.qRotation = ToHmdQuat(q);
		// no velocity: SteamVR must render exactly the pose we gave, the console reprojects
		m_pose.vecVelocity[0] = m_pose.vecVelocity[1] = m_pose.vecVelocity[2] = 0;
		m_pose.vecAngularVelocity[0] = m_pose.vecAngularVelocity[1] = m_pose.vecAngularVelocity[2] = 0;
		m_pose.poseTimeOffset = 0;
		m_pose.poseIsValid = connected;
		m_pose.deviceIsConnected = connected;
		m_pose.result = connected ? TrackingResult_Running_OK : TrackingResult_Running_OutOfRange;
		pose = m_pose;
	}
	if (id != k_unTrackedDeviceIndexInvalid) VRServerDriverHost()->TrackedDevicePoseUpdated(id, pose, sizeof(pose));
}

void Hmd::OnTracking(const Quat& q) {
	UpdatePose(q, true);
}

// ----------------------------------------------------- session -> SteamVR

void Hmd::SetSystemButton(bool down) {
	if (m_systemButton != k_ulInvalidInputComponentHandle)
		VRDriverInput()->UpdateBooleanComponent(m_systemButton, down, 0);
}

void Hmd::SetModelProperties(uint8_t model) {
	bool known = model < VR3DS_MODEL_COUNT;
	std::string slug = known ? kModelSlugs[model] : "generic";
	CVRPropertyHelpers* p = VRProperties();
	// The model number also selects the icon set in driver.vrresources.
	p->SetStringProperty(m_props, Prop_ModelNumber_String, known ? kModelNames[model] : "3DS VR");
	p->SetStringProperty(m_props, Prop_ManufacturerName_String, known ? "Nintendo" : "3DS VR");
	p->SetStringProperty(m_props, Prop_RenderModelName_String, ("{3dsvr}3dsvr_" + slug).c_str());
	std::string icons = "{3dsvr}/icons/" + slug + "_status_";
	p->SetStringProperty(m_props, Prop_NamedIconPathDeviceOff_String, (icons + "off.png").c_str());
	p->SetStringProperty(m_props, Prop_NamedIconPathDeviceSearching_String, (icons + "searching.gif").c_str());
	p->SetStringProperty(m_props, Prop_NamedIconPathDeviceSearchingAlert_String, (icons + "searching_alert.gif").c_str());
	p->SetStringProperty(m_props, Prop_NamedIconPathDeviceReady_String, (icons + "ready.png").c_str());
	p->SetStringProperty(m_props, Prop_NamedIconPathDeviceReadyAlert_String, (icons + "ready_alert.png").c_str());
	p->SetStringProperty(m_props, Prop_NamedIconPathDeviceNotReady_String, (icons + "error.png").c_str());
	p->SetStringProperty(m_props, Prop_NamedIconPathDeviceStandby_String, (icons + "standby.png").c_str());
	p->SetStringProperty(m_props, Prop_NamedIconPathDeviceStandbyAlert_String, (icons + "standby_alert.png").c_str());
	p->SetStringProperty(m_props, Prop_NamedIconPathDeviceAlertLow_String, (icons + "ready_low.png").c_str());
	m_appliedModel = model;
}

void Hmd::ApplyView(const SessionView& v) {
	uint32_t id = m_id;
	if (id == k_unTrackedDeviceIndexInvalid) return;
	CVRPropertyHelpers* p = VRProperties();

	if (v.connected && v.model != m_appliedModel) {
		SetModelProperties(v.model);
		Log("Headset model: %s", v.model < VR3DS_MODEL_COUNT ? kModelNames[v.model] : "?");
	}
	if (v.connected && (v.battery != m_appliedBattery || v.batteryFlags != m_appliedBatteryFlags)) {
		p->SetFloatProperty(m_props, Prop_DeviceBatteryPercentage_Float, v.battery / 100.0f);
		p->SetBoolProperty(m_props, Prop_DeviceIsCharging_Bool, (v.batteryFlags & VR3DS_BATT_CHARGING) != 0);
		m_appliedBattery = v.battery;
		m_appliedBatteryFlags = v.batteryFlags;
	}
	if (!m_appliedOnce || std::fabs(v.encTanX - m_tanX) > 1e-4f || std::fabs(v.encTanY - m_tanY) > 1e-4f) {
		m_tanX = v.encTanX;
		m_tanY = v.encTanY;
		HmdRect2_t eye = {{-v.encTanX, -v.encTanY}, {v.encTanX, v.encTanY}};
		VRServerDriverHost()->SetDisplayProjectionRaw(id, eye, eye);
	}
	if (std::fabs(v.ipd - m_appliedIpd) > 0.0005f) {
		m_appliedIpd = v.ipd;
		// Only the eye views move: the IPD shown by SteamVR stays the user's
		// (0 mm in 2D or a slider-scaled value would look broken).
		VRServerDriverHost()->SetDisplayEyeToHead(id, Translation(-v.ipd / 2), Translation(v.ipd / 2));
	}
	bool heightChanged;
	{
		std::lock_guard<std::mutex> lock(m_poseMutex);
		heightChanged = std::fabs(v.eyeHeight - m_eyeHeight) > 0.001f;
		m_eyeHeight = v.eyeHeight;
	}
	// seated apps start at the head's height
	if (heightChanged)
		p->SetStringProperty(m_props, Prop_DriverProvidedChaperoneJson_String, ChaperoneJson(v.eyeHeight).c_str());
	if (heightChanged || v.connected != m_appliedConnected) {
		Quat q;
		{
			std::lock_guard<std::mutex> lock(m_poseMutex);
			q = m_lastQ;
		}
		UpdatePose(q, v.connected);
		m_appliedConnected = v.connected;
	}
	m_applied = v;
	m_appliedOnce = true;
}

// SteamVR paces itself on these events, at the console screen's 60 Hz.
void Hmd::VsyncThread() {
	HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
	if (!timer) timer = CreateWaitableTimerW(nullptr, TRUE, nullptr);
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
	const uint64_t periodUs = (uint64_t)(1e6 / kRefreshHz);
	uint64_t next = NowUs() + periodUs;
	uint32_t lastVersion = 0;
	while (m_running) {
		uint64_t now = NowUs();
		if (next > now) {
			LARGE_INTEGER due;
			due.QuadPart = -(LONGLONG)((next - now) * 10);
			SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
			WaitForSingleObject(timer, 50);
		}
		now = NowUs();
		next += periodUs;
		if (now > next + periodUs) next = now + periodUs; // fell behind: resync

		if (m_id != k_unTrackedDeviceIndexInvalid) VRServerDriverHost()->VsyncEvent(0.0);
		{
			std::lock_guard<std::mutex> lock(m_vsyncMutex);
			m_vsyncCount++;
		}
		m_vsyncCv.notify_all();

		SessionView v = m_streamer->View();
		if (!m_appliedOnce || v.version != lastVersion) {
			lastVersion = v.version;
			ApplyView(v);
		}
	}
	CloseHandle(timer);
}

// --------------------------------------------------------------- display

void Hmd::GetWindowBounds(int32_t* x, int32_t* y, uint32_t* w, uint32_t* h) {
	*x = 0;
	*y = 0;
	*w = kEyeTargetW * 2;
	*h = kEyeTargetH;
}

void Hmd::GetRecommendedRenderTargetSize(uint32_t* w, uint32_t* h) {
	*w = kEyeTargetW;
	*h = kEyeTargetH;
}

void Hmd::GetEyeOutputViewport(EVREye eye, uint32_t* x, uint32_t* y, uint32_t* w, uint32_t* h) {
	*x = eye == Eye_Left ? 0 : kEyeTargetW;
	*y = 0;
	*w = kEyeTargetW;
	*h = kEyeTargetH;
}

void Hmd::GetProjectionRaw(EVREye, float* left, float* right, float* top, float* bottom) {
	float tx = m_tanX, ty = m_tanY;
	*left = -tx;
	*right = tx;
	*top = -ty;
	*bottom = ty;
}

DistortionCoordinates_t Hmd::ComputeDistortion(EVREye, float u, float v) {
	// no lens: the console shows the picture as is
	DistortionCoordinates_t c;
	c.rfRed[0] = c.rfGreen[0] = c.rfBlue[0] = u;
	c.rfRed[1] = c.rfGreen[1] = c.rfBlue[1] = v;
	return c;
}

bool Hmd::ComputeInverseDistortion(HmdVector2_t* r, EVREye, uint32_t, float u, float v) {
	r->v[0] = u;
	r->v[1] = v;
	return true;
}

// ----------------------------------------------------------- direct mode

void Hmd::CreateSwapTextureSet(uint32_t pid, const SwapTextureSetDesc_t* desc, SwapTextureSet_t* out) {
	m_compositor->CreateSwapTextureSet(pid, desc, out);
}

void Hmd::DestroySwapTextureSet(SharedTextureHandle_t handle) {
	m_compositor->DestroySwapTextureSet(handle);
}

void Hmd::DestroyAllSwapTextureSets(uint32_t pid) {
	m_compositor->DestroyAllSwapTextureSets(pid);
}

void Hmd::GetNextSwapTextureSetIndex(SharedTextureHandle_t[2], uint32_t (*indices)[2]) {
	(*indices)[0] = ((*indices)[0] + 1) % 3;
	(*indices)[1] = ((*indices)[1] + 1) % 3;
}

void Hmd::SubmitLayer(const SubmitLayerPerEye_t (&perEye)[2]) {
	m_compositor->SubmitLayer(perEye);
}

void Hmd::Present(SharedTextureHandle_t syncTexture) {
	FrameSlot slot;
	if (m_compositor->Present(syncTexture, m_streamer->Format(), &slot)) m_streamer->OnFrame(slot);
}

void Hmd::PostPresent(const Throttling_t*) {
	// hold the compositor until the next vsync
	std::unique_lock<std::mutex> lock(m_vsyncMutex);
	uint64_t count = m_vsyncCount;
	m_vsyncCv.wait_for(lock, std::chrono::milliseconds(40), [&] { return m_vsyncCount != count || !m_running; });
}
