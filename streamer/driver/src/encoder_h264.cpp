#include <windows.h>
#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <strmif.h>

#include <algorithm>
#include <cstring>
#include <deque>

#include "encoder.h"
#include "log.h"

namespace {

template <class T> void SafeRelease(T*& p) {
	if (p) p->Release();
	p = nullptr;
}

std::string Narrow(const wchar_t* w) {
	if (!w) return {};
	int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
	std::string s(n > 0 ? n - 1 : 0, '\0');
	if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
	return s;
}

void SetCodecValue(ICodecAPI* api, const GUID& key, UINT32 value) {
	VARIANT v;
	VariantInit(&v);
	v.vt = VT_UI4;
	v.ulVal = value;
	api->SetValue(&key, &v);
}

void SetCodecBool(ICodecAPI* api, const GUID& key, bool value) {
	VARIANT v;
	VariantInit(&v);
	v.vt = VT_BOOL;
	v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
	api->SetValue(&key, &v);
}

// BT.601 limited range, 2x2 chroma averaging.
void RgbaToNv12(const uint8_t* rgba, uint32_t pitch, int w, int h, uint8_t* y, uint8_t* uv) {
	for (int j = 0; j < h; j++) {
		const uint8_t* s = rgba + (size_t)j * pitch;
		uint8_t* d = y + (size_t)j * w;
		for (int i = 0; i < w; i++, s += 4) d[i] = (uint8_t)(((66 * s[0] + 129 * s[1] + 25 * s[2] + 128) >> 8) + 16);
	}
	for (int j = 0; j < h / 2; j++) {
		const uint8_t* r0 = rgba + (size_t)(2 * j) * pitch;
		const uint8_t* r1 = r0 + pitch;
		uint8_t* d = uv + (size_t)j * w;
		for (int i = 0; i < w / 2; i++) {
			const uint8_t *a = r0 + i * 8, *b = r1 + i * 8;
			int r = (a[0] + a[4] + b[0] + b[4] + 2) >> 2;
			int g = (a[1] + a[5] + b[1] + b[5] + 2) >> 2;
			int bl = (a[2] + a[6] + b[2] + b[6] + 2) >> 2;
			d[i * 2] = (uint8_t)(((-38 * r - 74 * g + 112 * bl + 128) >> 8) + 128);
			d[i * 2 + 1] = (uint8_t)(((112 * r - 94 * g - 18 * bl + 128) >> 8) + 128);
		}
	}
}

bool HasNal(const std::vector<uint8_t>& d, int type) {
	for (size_t i = 0; i + 3 < d.size(); i++)
		if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1 && (d[i + 3] & 0x1F) == type) return true;
	return false;
}

class H264EncoderMF : public VideoEncoder {
public:
	~H264EncoderMF() override {
		Shutdown();
		if (m_mfStarted) MFShutdown();
	}

	bool Init(int width, int height, int fps, int kbps) override {
		Shutdown();
		if (!m_mfStarted) {
			if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET))) return false;
			m_mfStarted = true;
		}
		m_w = width;
		m_h = height;
		m_fps = fps;
		m_kbps = kbps;

		MFT_REGISTER_TYPE_INFO inInfo = {MFMediaType_Video, MFVideoFormat_NV12};
		MFT_REGISTER_TYPE_INFO outInfo = {MFMediaType_Video, MFVideoFormat_H264};
		for (int pass = 0; pass < 2 && !m_mft; pass++) {
			UINT32 flags = MFT_ENUM_FLAG_SORTANDFILTER | (pass == 0 ? MFT_ENUM_FLAG_HARDWARE : MFT_ENUM_FLAG_SYNCMFT);
			IMFActivate** acts = nullptr;
			UINT32 count = 0;
			if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, &inInfo, &outInfo, &acts, &count))) continue;
			for (UINT32 i = 0; i < count && !m_mft; i++) {
				if (Configure(acts[i], pass == 0)) break;
				Shutdown();
			}
			for (UINT32 i = 0; i < count; i++) acts[i]->Release();
			CoTaskMemFree(acts);
		}
		if (!m_mft) {
			Log("H.264: no usable Media Foundation encoder");
			return false;
		}
		Log("H.264: %s, %dx%d @ %d fps, %d kbps", m_name.c_str(), width, height, fps, kbps);
		return true;
	}

	bool Encode(const uint8_t* rgba, uint32_t pitch, bool forceKey, EncodedFrame& out) override {
		if (!m_mft) return false;
		IMFSample* sample = MakeSample(rgba, pitch);
		if (!sample) return false;
		if (forceKey && m_codec) SetCodecValue(m_codec, CODECAPI_AVEncVideoForceKeyFrame, 1);

		bool ok = m_async ? EncodeAsync(sample, out) : EncodeSync(sample, out);
		sample->Release();
		if (!ok) return false;

		// the console decoder needs SPS/PPS in front of every key frame
		if (out.key && !HasNal(out.data, 7) && !m_seqHeader.empty())
			out.data.insert(out.data.begin(), m_seqHeader.begin(), m_seqHeader.end());
		return !out.data.empty();
	}

	std::string Name() const override { return m_name; }

private:
	bool Configure(IMFActivate* act, bool hardware) {
		wchar_t* friendly = nullptr;
		UINT32 len = 0;
		if (SUCCEEDED(act->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &friendly, &len))) {
			m_name = Narrow(friendly);
			CoTaskMemFree(friendly);
		} else {
			m_name = hardware ? "Hardware H.264 encoder" : "Software H.264 encoder";
		}
		if (FAILED(act->ActivateObject(IID_PPV_ARGS(&m_mft)))) return false;

		IMFAttributes* attrs = nullptr;
		if (SUCCEEDED(m_mft->GetAttributes(&attrs))) {
			UINT32 isAsync = 0;
			attrs->GetUINT32(MF_TRANSFORM_ASYNC, &isAsync);
			m_async = isAsync != 0;
			if (m_async) attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
			attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
			attrs->Release();
		}
		if (m_async && FAILED(m_mft->QueryInterface(IID_PPV_ARGS(&m_events)))) return false;

		DWORD inIds[1], outIds[1];
		if (SUCCEEDED(m_mft->GetStreamIDs(1, inIds, 1, outIds))) {
			m_inId = inIds[0];
			m_outId = outIds[0];
		}

		if (SUCCEEDED(m_mft->QueryInterface(IID_PPV_ARGS(&m_codec)))) {
			SetCodecBool(m_codec, CODECAPI_AVLowLatencyMode, true);
			SetCodecValue(m_codec, CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
			SetCodecValue(m_codec, CODECAPI_AVEncCommonMeanBitRate, (UINT32)m_kbps * 1000);
			// small VBV: no frame much bigger than its share of the bitrate
			SetCodecValue(m_codec, CODECAPI_AVEncCommonBufferSize, (UINT32)m_kbps * 1000 / m_fps * 3);
			SetCodecValue(m_codec, CODECAPI_AVEncMPVGOPSize, (UINT32)m_fps * 10); // key frames are also requested on loss
			SetCodecValue(m_codec, CODECAPI_AVEncMPVDefaultBPictureCount, 0);
			SetCodecBool(m_codec, CODECAPI_AVEncH264CABACEnable, false);
			SetCodecValue(m_codec, CODECAPI_AVEncSliceControlMode, 2); // macroblock rows per slice
			SetCodecValue(m_codec, CODECAPI_AVEncSliceControlSize, (UINT32)(m_h + 15) / 16);
			SetCodecValue(m_codec, CODECAPI_AVEncNumWorkerThreads, 1);
		}

		IMFMediaType* outType = nullptr;
		MFCreateMediaType(&outType);
		outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
		outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
		outType->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)m_kbps * 1000);
		MFSetAttributeSize(outType, MF_MT_FRAME_SIZE, m_w, m_h);
		MFSetAttributeRatio(outType, MF_MT_FRAME_RATE, m_fps, 1);
		MFSetAttributeRatio(outType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
		outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
		outType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base);
		outType->SetUINT32(MF_MT_MPEG2_LEVEL, eAVEncH264VLevel3_1);
		HRESULT hr = m_mft->SetOutputType(m_outId, outType, 0);
		outType->Release();
		if (FAILED(hr)) return false;

		IMFMediaType* inType = nullptr;
		MFCreateMediaType(&inType);
		inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
		inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
		MFSetAttributeSize(inType, MF_MT_FRAME_SIZE, m_w, m_h);
		MFSetAttributeRatio(inType, MF_MT_FRAME_RATE, m_fps, 1);
		MFSetAttributeRatio(inType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
		inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
		hr = m_mft->SetInputType(m_inId, inType, 0);
		inType->Release();
		if (FAILED(hr)) return false;

		ReadSequenceHeader();

		MFT_OUTPUT_STREAM_INFO info = {};
		m_mft->GetOutputStreamInfo(m_outId, &info);
		m_providesSamples = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
		m_outSize = std::max<DWORD>(info.cbSize, (DWORD)(m_w * m_h * 2));

		m_mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
		m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
		m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
		m_credits = 0;
		m_frame = 0;
		m_nv12.resize((size_t)m_w * m_h * 3 / 2);
		return true;
	}

	void ReadSequenceHeader() {
		IMFMediaType* cur = nullptr;
		if (FAILED(m_mft->GetOutputCurrentType(m_outId, &cur))) return;
		UINT32 size = 0;
		if (SUCCEEDED(cur->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size)) && size > 0) {
			m_seqHeader.resize(size);
			cur->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, m_seqHeader.data(), size, nullptr);
		}
		cur->Release();
	}

	void Shutdown() {
		if (m_mft) {
			m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
			m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
			IMFShutdown* sd = nullptr;
			if (SUCCEEDED(m_mft->QueryInterface(IID_PPV_ARGS(&sd)))) {
				sd->Shutdown();
				sd->Release();
			}
		}
		SafeRelease(m_codec);
		SafeRelease(m_events);
		SafeRelease(m_mft);
		m_seqHeader.clear();
		m_async = false;
		m_inId = m_outId = 0;
	}

	IMFSample* MakeSample(const uint8_t* rgba, uint32_t pitch) {
		RgbaToNv12(rgba, pitch, m_w, m_h, m_nv12.data(), m_nv12.data() + (size_t)m_w * m_h);
		IMFMediaBuffer* buf = nullptr;
		if (FAILED(MFCreateMemoryBuffer((DWORD)m_nv12.size(), &buf))) return nullptr;
		BYTE* p = nullptr;
		buf->Lock(&p, nullptr, nullptr);
		memcpy(p, m_nv12.data(), m_nv12.size());
		buf->Unlock();
		buf->SetCurrentLength((DWORD)m_nv12.size());
		IMFSample* s = nullptr;
		MFCreateSample(&s);
		s->AddBuffer(buf);
		buf->Release();
		LONGLONG dur = 10000000LL / m_fps;
		s->SetSampleTime(m_frame * dur);
		s->SetSampleDuration(dur);
		m_frame++;
		return s;
	}

	// Pulls one output sample. S_OK with data, MF_E_TRANSFORM_NEED_MORE_INPUT, or an error.
	HRESULT PullOutput(EncodedFrame& out) {
		for (int attempt = 0; attempt < 3; attempt++) {
			MFT_OUTPUT_DATA_BUFFER ob = {};
			ob.dwStreamID = m_outId;
			IMFSample* own = nullptr;
			if (!m_providesSamples) {
				IMFMediaBuffer* b = nullptr;
				MFCreateMemoryBuffer(m_outSize, &b);
				MFCreateSample(&own);
				own->AddBuffer(b);
				b->Release();
				ob.pSample = own;
			}
			DWORD status = 0;
			HRESULT hr = m_mft->ProcessOutput(0, 1, &ob, &status);
			if (ob.pEvents) ob.pEvents->Release();
			if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
				SafeRelease(own);
				IMFMediaType* t = nullptr;
				if (SUCCEEDED(m_mft->GetOutputAvailableType(m_outId, 0, &t))) {
					m_mft->SetOutputType(m_outId, t, 0);
					t->Release();
				}
				ReadSequenceHeader();
				continue;
			}
			if (FAILED(hr)) {
				SafeRelease(own);
				return hr;
			}
			IMFSample* s = ob.pSample;
			IMFMediaBuffer* cb = nullptr;
			if (s && SUCCEEDED(s->ConvertToContiguousBuffer(&cb))) {
				BYTE* p = nullptr;
				DWORD n = 0;
				cb->Lock(&p, nullptr, &n);
				out.data.insert(out.data.end(), p, p + n);
				cb->Unlock();
				cb->Release();
				UINT32 clean = 0;
				if (SUCCEEDED(s->GetUINT32(MFSampleExtension_CleanPoint, &clean)) && clean) out.key = true;
			}
			if (s && s != own) s->Release();
			SafeRelease(own);
			if (!out.key && HasNal(out.data, 5)) out.key = true;
			return S_OK;
		}
		return E_FAIL;
	}

	bool EncodeSync(IMFSample* sample, EncodedFrame& out) {
		HRESULT hr = m_mft->ProcessInput(m_inId, sample, 0);
		if (hr == MF_E_NOTACCEPTING) {
			PullOutput(out);
			hr = m_mft->ProcessInput(m_inId, sample, 0);
		}
		if (FAILED(hr)) return false;
		while (true) {
			hr = PullOutput(out);
			if (hr != S_OK) break;
		}
		return !out.data.empty();
	}

	// Waits for the next event of the async MFT (up to timeoutMs). Returns its type or 0.
	MediaEventType NextEvent(int timeoutMs) {
		uint64_t until = NowUs() + (uint64_t)timeoutMs * 1000;
		int spins = 0;
		while (true) {
			IMFMediaEvent* ev = nullptr;
			HRESULT hr = m_events->GetEvent(MF_EVENT_FLAG_NO_WAIT, &ev);
			if (SUCCEEDED(hr) && ev) {
				MediaEventType t = MEUnknown;
				ev->GetType(&t);
				ev->Release();
				return t;
			}
			if (hr != MF_E_NO_EVENTS_AVAILABLE) return 0;
			if (NowUs() > until) return 0;
			if (++spins < 200) SwitchToThread();
			else Sleep(1);
		}
	}

	bool EncodeAsync(IMFSample* sample, EncodedFrame& out) {
		while (m_credits == 0) {
			MediaEventType t = NextEvent(100);
			if (t == METransformNeedInput) m_credits++;
			else if (t == METransformHaveOutput) {
				EncodedFrame stale;
				PullOutput(stale); // a late frame: dropped, the next one follows
			} else if (t == 0) {
				return false;
			}
		}
		if (FAILED(m_mft->ProcessInput(m_inId, sample, 0))) return false;
		m_credits--;
		while (true) {
			MediaEventType t = NextEvent(60);
			if (t == METransformNeedInput) m_credits++;
			else if (t == METransformHaveOutput) return PullOutput(out) == S_OK;
			else if (t == 0) return false;
		}
	}

	bool m_mfStarted = false;
	IMFTransform* m_mft = nullptr;
	IMFMediaEventGenerator* m_events = nullptr;
	ICodecAPI* m_codec = nullptr;
	bool m_async = false;
	bool m_providesSamples = false;
	DWORD m_inId = 0, m_outId = 0, m_outSize = 0;
	int m_credits = 0;
	LONGLONG m_frame = 0;
	int m_w = 0, m_h = 0, m_fps = 60, m_kbps = 3000;
	std::vector<uint8_t> m_nv12;
	std::vector<uint8_t> m_seqHeader;
	std::string m_name;
};

} // namespace

std::unique_ptr<VideoEncoder> CreateH264Encoder() {
	return std::make_unique<H264EncoderMF>();
}
