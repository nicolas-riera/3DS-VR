#pragma once
#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

#include <openvr_driver.h>

#include "math_util.h"

// What the encoder needs: eye size, layout and the stream config it belongs to.
struct OutputFormat {
	uint16_t eyeWidth = 512, eyeHeight = 256;
	bool stereo = true; // eyes stacked vertically, left on top
	uint16_t configId = 0;
};

// One composited picture waiting in CPU-readable memory.
struct FrameSlot {
	int index = -1;
	OutputFormat format;
	Quat pose;             // head orientation the layers were rendered with
	uint64_t timestampUs = 0;
};

// Direct mode compositor: owns the swap textures SteamVR renders into, and
// draws the submitted layers (scene + overlays) into a small picture at the
// stream resolution, then copies it to a staging texture for the encoder.
class Compositor {
public:
	bool Init();
	void Shutdown();
	uint64_t AdapterLuid() const { return m_luid; }

	void CreateSwapTextureSet(uint32_t pid, const vr::IVRDriverDirectModeComponent::SwapTextureSetDesc_t* desc,
							  vr::IVRDriverDirectModeComponent::SwapTextureSet_t* out);
	void DestroySwapTextureSet(vr::SharedTextureHandle_t handle);
	void DestroyAllSwapTextureSets(uint32_t pid);
	void SubmitLayer(const vr::IVRDriverDirectModeComponent::SubmitLayerPerEye_t (&perEye)[2]);

	// Composites the submitted layers. Returns true and fills `slot` when a
	// picture is ready for the encoder (then call Map / Release on it).
	bool Present(vr::SharedTextureHandle_t syncTexture, const OutputFormat& format, FrameSlot* slot);

	bool Map(const FrameSlot& slot, const uint8_t** data, uint32_t* pitch);
	void Release(const FrameSlot& slot); // unmaps and frees the slot

private:
	struct SwapTexture {
		Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
		Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
		bool encodeSrgb = false; // linear content: convert to sRGB when compositing
		uint32_t pid = 0;
		vr::SharedTextureHandle_t setId = 0; // first handle of its set
	};
	struct Layer {
		vr::SharedTextureHandle_t texture[2];
		vr::VRTextureBounds_t bounds[2];
		vr::HmdMatrix34_t pose;
	};
	static constexpr int kSlots = 3;

	bool CreatePipeline();
	bool EnsureOutput(const OutputFormat& f);
	ID3D11Texture2D* SyncTexture(vr::SharedTextureHandle_t h);

	Microsoft::WRL::ComPtr<ID3D11Device> m_device;
	Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_ctx;
	std::mutex m_ctxMutex;
	uint64_t m_luid = 0;

	Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vs;
	Microsoft::WRL::ComPtr<ID3D11PixelShader> m_ps;
	Microsoft::WRL::ComPtr<ID3D11Buffer> m_cb;
	Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;
	Microsoft::WRL::ComPtr<ID3D11BlendState> m_blendOpaque, m_blendAlpha;
	Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_raster;

	Microsoft::WRL::ComPtr<ID3D11Texture2D> m_output;
	Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_outputRtv;
	Microsoft::WRL::ComPtr<ID3D11Texture2D> m_staging[kSlots];
	bool m_slotBusy[kSlots] = {};
	bool m_slotMapped[kSlots] = {};
	int m_nextSlot = 0;
	OutputFormat m_outFormat;
	bool m_hasOutput = false;

	std::mutex m_texMutex;
	std::map<vr::SharedTextureHandle_t, SwapTexture> m_textures;
	std::map<vr::SharedTextureHandle_t, Microsoft::WRL::ComPtr<ID3D11Texture2D>> m_syncTextures;
	std::vector<Layer> m_layers;
};
