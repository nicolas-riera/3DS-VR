#include "compositor.h"

#include <d3dcompiler.h>
#include <dxgi1_2.h>

#include <cstring>

#include "log.h"

using Microsoft::WRL::ComPtr;

static const char kShader[] = R"(
cbuffer Params : register(b0) {
	float4 uvRect;      // uMin, vMin, uMax, vMax
	float encodeSrgb;
	float3 pad;
};
Texture2D tex : register(t0);
SamplerState smp : register(s0);

struct VsOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VsOut VSMain(uint id : SV_VertexID) {
	float2 p = float2(id & 1, id >> 1);
	VsOut o;
	o.pos = float4(p.x * 2 - 1, 1 - p.y * 2, 0, 1);
	o.uv = lerp(uvRect.xy, uvRect.zw, p);
	return o;
}

float3 LinearToSrgb(float3 c) {
	c = saturate(c);
	float3 lo = c * 12.92;
	float3 hi = 1.055 * pow(c, 1.0 / 2.4) - 0.055;
	return lerp(lo, hi, step(0.0031308, c));
}

float4 PSMain(VsOut i) : SV_Target {
	float4 c = tex.Sample(smp, i.uv);
	if (encodeSrgb > 0.5) c.rgb = LinearToSrgb(c.rgb);
	return c;
}
)";

struct ShaderParams {
	float uvRect[4];
	float encodeSrgb;
	float pad[3];
};

// The swap textures may be typeless: pick the view format SteamVR means.
static DXGI_FORMAT ViewFormat(DXGI_FORMAT f, bool* encodeSrgb) {
	*encodeSrgb = false;
	switch (f) {
	case DXGI_FORMAT_R8G8B8A8_TYPELESS:
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
		*encodeSrgb = true; // sampling decodes to linear
		return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	case DXGI_FORMAT_B8G8R8A8_TYPELESS:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
		*encodeSrgb = true;
		return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
	case DXGI_FORMAT_B8G8R8X8_TYPELESS:
	case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
		*encodeSrgb = true;
		return DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
	case DXGI_FORMAT_R16G16B16A16_TYPELESS:
	case DXGI_FORMAT_R16G16B16A16_FLOAT:
		*encodeSrgb = true; // linear float
		return DXGI_FORMAT_R16G16B16A16_FLOAT;
	case DXGI_FORMAT_R10G10B10A2_TYPELESS:
		return DXGI_FORMAT_R10G10B10A2_UNORM;
	default:
		return f; // 8-bit UNORM: already gamma encoded
	}
}

bool Compositor::Init() {
	ComPtr<IDXGIFactory1> factory;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
	ComPtr<IDXGIAdapter1> adapter;
	if (FAILED(factory->EnumAdapters1(0, &adapter))) return false;
	DXGI_ADAPTER_DESC1 desc = {};
	adapter->GetDesc1(&desc);
	m_luid = ((uint64_t)desc.AdapterLuid.HighPart << 32) | desc.AdapterLuid.LowPart;

	D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
	UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
	HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels, 2,
								   D3D11_SDK_VERSION, &m_device, nullptr, &m_ctx);
	if (FAILED(hr))
		hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels + 1, 1,
							   D3D11_SDK_VERSION, &m_device, nullptr, &m_ctx);
	if (FAILED(hr)) {
		Log("D3D11CreateDevice failed: 0x%08X", (unsigned)hr);
		return false;
	}
	char name[128];
	WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
	Log("GPU: %s", name);
	return CreatePipeline();
}

void Compositor::Shutdown() {
	std::lock_guard<std::mutex> lock(m_ctxMutex);
	std::lock_guard<std::mutex> lock2(m_texMutex);
	m_textures.clear();
	m_syncTextures.clear();
	for (auto& s : m_staging) s.Reset();
	m_output.Reset();
	m_outputRtv.Reset();
	if (m_ctx) m_ctx->ClearState();
}

bool Compositor::CreatePipeline() {
	ComPtr<ID3DBlob> vsb, psb, err;
	if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "compositor", nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vsb,
						  &err)) ||
		FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "compositor", nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &psb,
						  &err))) {
		Log("Shader compilation failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
		return false;
	}
	m_device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &m_vs);
	m_device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &m_ps);

	D3D11_BUFFER_DESC cbd = {};
	cbd.ByteWidth = sizeof(ShaderParams);
	cbd.Usage = D3D11_USAGE_DEFAULT;
	cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	m_device->CreateBuffer(&cbd, nullptr, &m_cb);

	D3D11_SAMPLER_DESC sd = {};
	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.MaxLOD = D3D11_FLOAT32_MAX;
	m_device->CreateSamplerState(&sd, &m_sampler);

	D3D11_BLEND_DESC bd = {};
	bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	m_device->CreateBlendState(&bd, &m_blendOpaque);
	bd.RenderTarget[0].BlendEnable = TRUE;
	bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
	bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
	bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
	bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
	bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
	bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	m_device->CreateBlendState(&bd, &m_blendAlpha);

	D3D11_RASTERIZER_DESC rd = {};
	rd.FillMode = D3D11_FILL_SOLID;
	rd.CullMode = D3D11_CULL_NONE;
	m_device->CreateRasterizerState(&rd, &m_raster);
	return m_vs && m_ps && m_cb && m_sampler && m_blendOpaque && m_blendAlpha && m_raster;
}

void Compositor::CreateSwapTextureSet(uint32_t pid, const vr::IVRDriverDirectModeComponent::SwapTextureSetDesc_t* desc,
									  vr::IVRDriverDirectModeComponent::SwapTextureSet_t* out) {
	memset(out, 0, sizeof(*out));
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = desc->nWidth;
	td.Height = desc->nHeight;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = (DXGI_FORMAT)desc->nFormat;
	td.SampleDesc.Count = 1; // we only ever sample them
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	td.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

	std::lock_guard<std::mutex> lock(m_texMutex);
	vr::SharedTextureHandle_t first = 0;
	for (int i = 0; i < 3; i++) {
		SwapTexture st;
		if (FAILED(m_device->CreateTexture2D(&td, nullptr, &st.texture))) {
			Log("CreateSwapTextureSet: CreateTexture2D failed (%ux%u, format %u)", td.Width, td.Height, td.Format);
			return;
		}
		D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
		sv.Format = ViewFormat(td.Format, &st.encodeSrgb);
		sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		sv.Texture2D.MipLevels = 1;
		m_device->CreateShaderResourceView(st.texture.Get(), &sv, &st.srv);

		ComPtr<IDXGIResource> res;
		HANDLE handle = nullptr;
		if (FAILED(st.texture.As(&res)) || FAILED(res->GetSharedHandle(&handle))) return;
		vr::SharedTextureHandle_t h = (vr::SharedTextureHandle_t)handle;
		if (i == 0) first = h;
		st.pid = pid;
		st.setId = first;
		m_textures[h] = st;
		out->rSharedTextureHandles[i] = h;
	}
}

void Compositor::DestroySwapTextureSet(vr::SharedTextureHandle_t handle) {
	std::lock_guard<std::mutex> lock(m_texMutex);
	auto it = m_textures.find(handle);
	if (it == m_textures.end()) return;
	vr::SharedTextureHandle_t set = it->second.setId;
	for (auto i = m_textures.begin(); i != m_textures.end();) {
		if (i->second.setId == set) i = m_textures.erase(i);
		else ++i;
	}
}

void Compositor::DestroyAllSwapTextureSets(uint32_t pid) {
	std::lock_guard<std::mutex> lock(m_texMutex);
	for (auto i = m_textures.begin(); i != m_textures.end();) {
		if (i->second.pid == pid) i = m_textures.erase(i);
		else ++i;
	}
}

void Compositor::SubmitLayer(const vr::IVRDriverDirectModeComponent::SubmitLayerPerEye_t (&perEye)[2]) {
	std::lock_guard<std::mutex> lock(m_texMutex);
	if (m_layers.size() >= 8) return;
	Layer l;
	for (int e = 0; e < 2; e++) {
		l.texture[e] = perEye[e].hTexture;
		l.bounds[e] = perEye[e].bounds;
	}
	l.pose = perEye[0].mHmdPose;
	m_layers.push_back(l);
}

ID3D11Texture2D* Compositor::SyncTexture(vr::SharedTextureHandle_t h) {
	auto it = m_syncTextures.find(h);
	if (it != m_syncTextures.end()) return it->second.Get();
	ComPtr<ID3D11Texture2D> tex;
	if (FAILED(m_device->OpenSharedResource((HANDLE)h, IID_PPV_ARGS(&tex)))) return nullptr;
	m_syncTextures[h] = tex;
	return tex.Get();
}

bool Compositor::EnsureOutput(const OutputFormat& f) {
	if (m_hasOutput && f.eyeWidth == m_outFormat.eyeWidth && f.height == m_outFormat.height) return true;
	// wait until the encoder is done with the old pictures
	for (bool busy : m_slotBusy)
		if (busy) return false;

	D3D11_TEXTURE2D_DESC td = {};
	td.Width = f.eyeWidth;
	td.Height = f.height;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_RENDER_TARGET;
	m_output.Reset();
	m_outputRtv.Reset();
	if (FAILED(m_device->CreateTexture2D(&td, nullptr, &m_output))) return false;
	m_device->CreateRenderTargetView(m_output.Get(), nullptr, &m_outputRtv);

	td.Usage = D3D11_USAGE_STAGING;
	td.BindFlags = 0;
	td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	for (auto& s : m_staging) {
		s.Reset();
		if (FAILED(m_device->CreateTexture2D(&td, nullptr, &s))) return false;
	}
	m_outFormat = f;
	m_hasOutput = true;
	Log("Compositor output: %ux%u", td.Width, td.Height);
	return true;
}

bool Compositor::Present(vr::SharedTextureHandle_t syncTexture, const OutputFormat& format, FrameSlot* slot) {
	std::vector<Layer> layers;
	{
		std::lock_guard<std::mutex> lock(m_texMutex);
		layers.swap(m_layers);
	}
	if (layers.empty() || format.configId == 0) return false; // nobody to stream to

	std::lock_guard<std::mutex> lock(m_ctxMutex);
	if (!EnsureOutput(format)) return false;

	int s = -1;
	for (int i = 0; i < kSlots; i++) {
		int k = (m_nextSlot + i) % kSlots;
		if (!m_slotBusy[k]) {
			s = k;
			break;
		}
	}
	if (s < 0) return false; // the encoder is behind: drop this picture
	m_nextSlot = (s + 1) % kSlots;

	// The sync texture's keyed mutex tells us the compositor finished rendering.
	ID3D11Texture2D* sync = SyncTexture(syncTexture);
	ComPtr<IDXGIKeyedMutex> km;
	if (sync && SUCCEEDED(sync->QueryInterface(IID_PPV_ARGS(&km)))) {
		if (km->AcquireSync(0, 10) != S_OK) return false;
	}

	float clear[4] = {0, 0, 0, 1};
	m_ctx->ClearRenderTargetView(m_outputRtv.Get(), clear);
	m_ctx->OMSetRenderTargets(1, m_outputRtv.GetAddressOf(), nullptr);
	m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	m_ctx->IASetInputLayout(nullptr);
	m_ctx->VSSetShader(m_vs.Get(), nullptr, 0);
	m_ctx->PSSetShader(m_ps.Get(), nullptr, 0);
	m_ctx->PSSetSamplers(0, 1, m_sampler.GetAddressOf());
	m_ctx->VSSetConstantBuffers(0, 1, m_cb.GetAddressOf());
	m_ctx->PSSetConstantBuffers(0, 1, m_cb.GetAddressOf());
	m_ctx->RSSetState(m_raster.Get());

	int eyes = format.stereo ? 2 : 1;
	{
		std::lock_guard<std::mutex> texLock(m_texMutex);
		for (size_t li = 0; li < layers.size(); li++) {
			const Layer& l = layers[li];
			float factor[4] = {0, 0, 0, 0};
			m_ctx->OMSetBlendState(li == 0 ? m_blendOpaque.Get() : m_blendAlpha.Get(), factor, 0xFFFFFFFF);
			for (int e = 0; e < eyes; e++) {
				auto it = m_textures.find(l.texture[e]);
				if (it == m_textures.end()) continue;
				D3D11_VIEWPORT vp = {0, (float)(e * format.eyeHeight), (float)format.eyeWidth, (float)format.eyeHeight,
									 0, 1};
				m_ctx->RSSetViewports(1, &vp);
				ShaderParams p = {};
				p.uvRect[0] = l.bounds[e].uMin;
				p.uvRect[1] = l.bounds[e].vMin;
				p.uvRect[2] = l.bounds[e].uMax;
				p.uvRect[3] = l.bounds[e].vMax;
				p.encodeSrgb = it->second.encodeSrgb ? 1.0f : 0.0f;
				m_ctx->UpdateSubresource(m_cb.Get(), 0, nullptr, &p, 0, 0);
				m_ctx->PSSetShaderResources(0, 1, it->second.srv.GetAddressOf());
				m_ctx->Draw(4, 0);
			}
		}
	}
	ID3D11ShaderResourceView* nullSrv = nullptr;
	m_ctx->PSSetShaderResources(0, 1, &nullSrv);

	if (km) km->ReleaseSync(0);

	m_ctx->CopyResource(m_staging[s].Get(), m_output.Get());
	m_ctx->Flush();
	m_slotBusy[s] = true;

	slot->index = s;
	slot->format = format;
	slot->pose = QuatFromMatrix(layers[0].pose);
	slot->timestampUs = NowUs();
	return true;
}

bool Compositor::Map(const FrameSlot& slot, const uint8_t** data, uint32_t* pitch) {
	std::lock_guard<std::mutex> lock(m_ctxMutex);
	if (slot.index < 0 || slot.index >= kSlots || !m_staging[slot.index]) return false;
	D3D11_MAPPED_SUBRESOURCE m;
	if (FAILED(m_ctx->Map(m_staging[slot.index].Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
	m_slotMapped[slot.index] = true;
	*data = (const uint8_t*)m.pData;
	*pitch = m.RowPitch;
	return true;
}

void Compositor::Release(const FrameSlot& slot) {
	std::lock_guard<std::mutex> lock(m_ctxMutex);
	if (slot.index < 0 || slot.index >= kSlots) return;
	if (m_slotMapped[slot.index] && m_staging[slot.index]) m_ctx->Unmap(m_staging[slot.index].Get(), 0);
	m_slotMapped[slot.index] = false;
	m_slotBusy[slot.index] = false;
}
