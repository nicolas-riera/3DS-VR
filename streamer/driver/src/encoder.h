#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct EncodedFrame {
	std::vector<uint8_t> data;
	bool key = false;
};

// Encodes RGBA8 pictures (top-down rows).
class VideoEncoder {
public:
	virtual ~VideoEncoder() = default;
	virtual bool Init(int width, int height, int fps, int kbps) = 0;
	virtual bool Encode(const uint8_t* rgba, uint32_t pitch, bool forceKey, EncodedFrame& out) = 0;
	virtual std::string Name() const = 0;
};

// Media Foundation H.264 (hardware encoder when available), constrained
// baseline, low-latency mode, CBR, no B-frames, one slice per frame.
std::unique_ptr<VideoEncoder> CreateH264Encoder();
// Windows Imaging Component baseline JPEG, quality steered towards the bitrate.
std::unique_ptr<VideoEncoder> CreateJpegEncoder();
