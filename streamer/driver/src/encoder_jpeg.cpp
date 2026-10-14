#include <windows.h>
#include <wincodec.h>

#include <algorithm>
#include <vector>

#include "encoder.h"
#include "log.h"

namespace {

template <class T> void SafeRelease(T*& p) {
	if (p) p->Release();
	p = nullptr;
}

// Old 3DS family: no hardware decoder, but a JPEG decodes fast enough on the
// ARM11 and every frame stands alone, so a lost packet costs one frame only.
class JpegEncoderWic : public VideoEncoder {
public:
	~JpegEncoderWic() override {
		SafeRelease(m_factory);
		if (m_com) CoUninitialize();
	}

	bool Init(int width, int height, int fps, int kbps) override {
		if (!m_factory) {
			m_com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
			if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_factory))))
				return false;
		}
		m_w = width;
		m_h = height;
		m_targetBytes = std::max(2000, kbps * 1000 / 8 / std::max(fps, 1));
		m_bgr.resize((size_t)width * height * 3);
		Log("JPEG: %dx%d @ %d fps, %d kbps (%d bytes per frame)", width, height, fps, kbps, m_targetBytes);
		return true;
	}

	bool Encode(const uint8_t* rgba, uint32_t pitch, bool, EncodedFrame& out) override {
		for (int y = 0; y < m_h; y++) {
			const uint8_t* s = rgba + (size_t)y * pitch;
			uint8_t* d = m_bgr.data() + (size_t)y * m_w * 3;
			for (int x = 0; x < m_w; x++, s += 4, d += 3) {
				d[0] = s[2];
				d[1] = s[1];
				d[2] = s[0];
			}
		}
		if (!EncodeOnce(out.data)) return false;
		out.key = true;
		// steer the quality towards the bitrate budget
		float err = (float)((int)out.data.size() - m_targetBytes) / (float)m_targetBytes;
		m_quality = std::clamp(m_quality - err * 0.04f, 0.25f, 0.92f);
		return true;
	}

	std::string Name() const override { return "Windows Imaging Component JPEG"; }

private:
	bool EncodeOnce(std::vector<uint8_t>& out) {
		IStream* stream = nullptr;
		IWICBitmapEncoder* enc = nullptr;
		IWICBitmapFrameEncode* frame = nullptr;
		IPropertyBag2* props = nullptr;
		bool ok = false;
		do {
			if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) break;
			if (FAILED(m_factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &enc))) break;
			if (FAILED(enc->Initialize(stream, WICBitmapEncoderNoCache))) break;
			if (FAILED(enc->CreateNewFrame(&frame, &props))) break;
			PROPBAG2 opt[2] = {};
			VARIANT val[2];
			opt[0].pstrName = (LPOLESTR)L"ImageQuality";
			VariantInit(&val[0]);
			val[0].vt = VT_R4;
			val[0].fltVal = m_quality;
			opt[1].pstrName = (LPOLESTR)L"JpegYCrCbSubsampling";
			VariantInit(&val[1]);
			val[1].vt = VT_UI1;
			val[1].bVal = WICJpegYCrCbSubsampling420;
			props->Write(2, opt, val);
			if (FAILED(frame->Initialize(props))) break;
			if (FAILED(frame->SetSize(m_w, m_h))) break;
			WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
			if (FAILED(frame->SetPixelFormat(&fmt)) || fmt != GUID_WICPixelFormat24bppBGR) break;
			if (FAILED(frame->WritePixels(m_h, m_w * 3, (UINT)m_bgr.size(), m_bgr.data()))) break;
			if (FAILED(frame->Commit()) || FAILED(enc->Commit())) break;
			STATSTG st = {};
			if (FAILED(stream->Stat(&st, STATFLAG_NONAME))) break;
			HGLOBAL mem = nullptr;
			if (FAILED(GetHGlobalFromStream(stream, &mem))) break;
			const uint8_t* p = (const uint8_t*)GlobalLock(mem);
			if (!p) break;
			out.assign(p, p + st.cbSize.QuadPart);
			GlobalUnlock(mem);
			ok = true;
		} while (false);
		SafeRelease(props);
		SafeRelease(frame);
		SafeRelease(enc);
		SafeRelease(stream);
		return ok;
	}

	bool m_com = false;
	IWICImagingFactory* m_factory = nullptr;
	int m_w = 0, m_h = 0;
	int m_targetBytes = 10000;
	float m_quality = 0.7f;
	std::vector<uint8_t> m_bgr;
};

} // namespace

std::unique_ptr<VideoEncoder> CreateJpegEncoder() {
	return std::make_unique<JpegEncoderWic>();
}
