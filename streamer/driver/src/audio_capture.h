#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

// Captures what the PC plays (WASAPI loopback on the default output device),
// resamples it to 32 kHz stereo and encodes 16 ms IMA-ADPCM packets.
class AudioCapture {
public:
	// packet: a complete VR3DS_PKT_AUDIO datagram
	using Sink = std::function<void(const uint8_t* packet, int size)>;

	~AudioCapture() { Stop(); }
	void Start(Sink sink);
	void Stop();
	bool Running() const { return m_capturing; }

private:
	void Thread();
	bool CaptureSession(); // returns when the device changes or Stop() is called
	void Push(const float* stereo, uint32_t frames, uint32_t rate);
	void EncodePacket();

	Sink m_sink;
	std::thread m_thread;
	std::atomic<bool> m_running{false};
	std::atomic<bool> m_capturing{false};

	// resampler
	double m_pos = 0.0;
	float m_prev[2] = {0, 0};
	std::vector<int16_t> m_pcm; // interleaved, at 32 kHz

	// IMA-ADPCM state
	int m_pred[2] = {0, 0};
	int m_index[2] = {0, 0};
	uint32_t m_seq = 0;
};
