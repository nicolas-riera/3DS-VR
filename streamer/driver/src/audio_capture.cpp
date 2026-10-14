#include "audio_capture.h"

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <avrt.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "log.h"
#include "vr3ds_protocol.h"

template <class T> static void SafeRelease(T*& p) {
	if (p) p->Release();
	p = nullptr;
}

static const int kStepTable[89] = {
	7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
	31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
	130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
	544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
	2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
	9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
static const int kIndexTable[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

static uint8_t ImaEncode(int sample, int& pred, int& index) {
	int step = kStepTable[index];
	int diff = sample - pred;
	uint8_t nibble = 0;
	if (diff < 0) {
		nibble = 8;
		diff = -diff;
	}
	int delta = step >> 3;
	if (diff >= step) {
		nibble |= 4;
		diff -= step;
		delta += step;
	}
	step >>= 1;
	if (diff >= step) {
		nibble |= 2;
		diff -= step;
		delta += step;
	}
	step >>= 1;
	if (diff >= step) {
		nibble |= 1;
		delta += step;
	}
	pred += (nibble & 8) ? -delta : delta;
	pred = std::clamp(pred, -32768, 32767);
	index = std::clamp(index + kIndexTable[nibble], 0, 88);
	return nibble;
}

void AudioCapture::Start(Sink sink) {
	if (m_running) return;
	m_sink = std::move(sink);
	m_running = true;
	m_thread = std::thread(&AudioCapture::Thread, this);
}

void AudioCapture::Stop() {
	if (!m_running) return;
	m_running = false;
	if (m_thread.joinable()) m_thread.join();
	m_capturing = false;
}

void AudioCapture::Thread() {
	CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	DWORD taskIndex = 0;
	HANDLE avrt = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
	while (m_running) {
		if (!CaptureSession()) {
			// no device (or it failed): try again a bit later
			for (int i = 0; i < 20 && m_running; i++) Sleep(100);
		}
	}
	if (avrt) AvRevertMmThreadCharacteristics(avrt);
	CoUninitialize();
}

static std::wstring DefaultDeviceId(IMMDeviceEnumerator* en) {
	IMMDevice* dev = nullptr;
	std::wstring id;
	if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) {
		LPWSTR s = nullptr;
		if (SUCCEEDED(dev->GetId(&s))) {
			id = s;
			CoTaskMemFree(s);
		}
		dev->Release();
	}
	return id;
}

bool AudioCapture::CaptureSession() {
	IMMDeviceEnumerator* en = nullptr;
	IMMDevice* dev = nullptr;
	IAudioClient* client = nullptr;
	IAudioCaptureClient* capture = nullptr;
	WAVEFORMATEX* fmt = nullptr;
	bool ok = false;
	std::wstring deviceId;

	do {
		if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) break;
		if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) break;
		deviceId = DefaultDeviceId(en);
		if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client))) break;
		if (FAILED(client->GetMixFormat(&fmt))) break;
		// 100 ms buffer, shared mode, loopback of the render endpoint
		if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 1000000, 0, fmt, nullptr)))
			break;
		if (FAILED(client->GetService(IID_PPV_ARGS(&capture)))) break;
		if (FAILED(client->Start())) break;
		ok = true;
	} while (false);

	if (ok) {
		bool isFloat = fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
					   (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
						((WAVEFORMATEXTENSIBLE*)fmt)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
		int channels = fmt->nChannels;
		int bits = fmt->wBitsPerSample;
		uint32_t rate = fmt->nSamplesPerSec;
		Log("Audio: capturing the default output (%u Hz, %d ch, %d-bit %s)", rate, channels, bits,
			isFloat ? "float" : "int");
		m_capturing = true;
		m_pcm.clear();
		std::vector<float> stereo;
		uint64_t lastDeviceCheck = NowUs();

		while (m_running) {
			Sleep(5);
			UINT32 packet = 0;
			HRESULT hr = capture->GetNextPacketSize(&packet);
			if (FAILED(hr)) break; // device invalidated
			while (packet > 0) {
				BYTE* data = nullptr;
				UINT32 frames = 0;
				DWORD flags = 0;
				if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
				stereo.resize(frames * 2);
				for (UINT32 i = 0; i < frames; i++) {
					float l = 0, r = 0;
					if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
						if (isFloat && bits == 32) {
							const float* s = (const float*)data + i * channels;
							l = s[0];
							r = channels > 1 ? s[1] : s[0];
						} else if (bits == 16) {
							const int16_t* s = (const int16_t*)data + i * channels;
							l = s[0] / 32768.0f;
							r = (channels > 1 ? s[1] : s[0]) / 32768.0f;
						} else if (bits == 32) {
							const int32_t* s = (const int32_t*)data + i * channels;
							l = s[0] / 2147483648.0f;
							r = (channels > 1 ? s[1] : s[0]) / 2147483648.0f;
						}
					}
					stereo[i * 2] = l;
					stereo[i * 2 + 1] = r;
				}
				capture->ReleaseBuffer(frames);
				Push(stereo.data(), frames, rate);
				if (FAILED(capture->GetNextPacketSize(&packet))) break;
			}
			// follow the user when they switch the default output device
			uint64_t now = NowUs();
			if (now - lastDeviceCheck > 2000000) {
				lastDeviceCheck = now;
				if (DefaultDeviceId(en) != deviceId) break;
			}
		}
		client->Stop();
		m_capturing = false;
	}

	if (fmt) CoTaskMemFree(fmt);
	SafeRelease(capture);
	SafeRelease(client);
	SafeRelease(dev);
	SafeRelease(en);
	return ok;
}

void AudioCapture::Push(const float* stereo, uint32_t frames, uint32_t rate) {
	// linear interpolation to 32 kHz; m_pos is the read position relative to the
	// start of this block (-1 .. 0 = between the previous block's last sample and the first one)
	double step = (double)rate / VR3DS_AUDIO_RATE;
	while (m_pos < (double)frames - 1.0) {
		int i = (int)std::floor(m_pos);
		double t = m_pos - i;
		float l0, r0;
		if (i < 0) {
			l0 = m_prev[0];
			r0 = m_prev[1];
		} else {
			l0 = stereo[i * 2];
			r0 = stereo[i * 2 + 1];
		}
		float l1 = stereo[(i + 1) * 2], r1 = stereo[(i + 1) * 2 + 1];
		float l = (float)(l0 + (l1 - l0) * t), r = (float)(r0 + (r1 - r0) * t);
		m_pcm.push_back((int16_t)std::clamp((int)std::lround(l * 32767.0f), -32768, 32767));
		m_pcm.push_back((int16_t)std::clamp((int)std::lround(r * 32767.0f), -32768, 32767));
		m_pos += step;
		if (m_pcm.size() >= VR3DS_AUDIO_FRAMES * 2) EncodePacket();
	}
	if (frames > 0) {
		m_prev[0] = stereo[(frames - 1) * 2];
		m_prev[1] = stereo[(frames - 1) * 2 + 1];
		m_pos -= frames;
	}
}

void AudioCapture::EncodePacket() {
	uint8_t packet[sizeof(Vr3dsAudioHeader) + VR3DS_AUDIO_FRAMES];
	Vr3dsAudioHeader* h = (Vr3dsAudioHeader*)packet;
	vr3ds_header(&h->h, VR3DS_PKT_AUDIO);
	h->seq = m_seq++;
	h->frames = VR3DS_AUDIO_FRAMES;
	h->predictor[0] = (int16_t)m_pred[0];
	h->predictor[1] = (int16_t)m_pred[1];
	h->stepIndex[0] = (uint8_t)m_index[0];
	h->stepIndex[1] = (uint8_t)m_index[1];
	uint8_t* out = packet + sizeof(Vr3dsAudioHeader);
	for (int i = 0; i < VR3DS_AUDIO_FRAMES; i++) {
		uint8_t lo = ImaEncode(m_pcm[i * 2], m_pred[0], m_index[0]);
		uint8_t hi = ImaEncode(m_pcm[i * 2 + 1], m_pred[1], m_index[1]);
		out[i] = (uint8_t)(lo | (hi << 4));
	}
	m_pcm.erase(m_pcm.begin(), m_pcm.begin() + VR3DS_AUDIO_FRAMES * 2);
	if (m_sink) m_sink(packet, (int)sizeof(packet));
}
