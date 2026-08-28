#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace opennova::bink {

// Random-access byte source. The callback must either fill the complete
// requested range and return true, or return false without relying on a
// partially-filled destination. The source and anything captured by read_at
// must outlive the BinkMovie.
struct BinkSource {
	uint64_t size = 0;
	std::function<bool(uint64_t offset, uint8_t *destination, size_t size)> read_at;
};

struct BinkInfo {
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t frame_count = 0;
	uint32_t largest_frame = 0;
	uint32_t fps_numerator = 0;
	uint32_t fps_denominator = 0;
	uint32_t file_flags = 0;
	uint32_t audio_track_count = 0;

	double frames_per_second() const;
};

// One tightly-packed, top-down RGBA8 frame. BinkMovie owns and reuses this
// storage; callers that need a longer lifetime must copy it.
struct BinkFrame {
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t stride = 0;
	std::vector<uint8_t> rgba;
};

enum class BinkStatus {
	frame_ready,
	end_of_stream,
	io_error,
	invalid_data,
	unsupported,
};

// The retail YUV -> RGB conversion of one pixel (binkw32.dll's YUV_init
// tables + BINKSURFACE32 blit; the law is documented at the definition).
// Exposed so the conversion is pinnable without a decoded stream.
void yuv_to_rgb(uint8_t y, uint8_t u, uint8_t v, uint8_t &r, uint8_t &g, uint8_t &b);

// Portable BIKi video-only decoder. This deliberately exposes no container,
// bundle, YUV-plane, or transform machinery: those are one implementation
// detail behind the sequential movie interface used by the movie binding.
class BinkMovie final {
public:
	static std::unique_ptr<BinkMovie> open(
			BinkSource source, std::string *error = nullptr);
	~BinkMovie();

	BinkMovie(const BinkMovie &) = delete;
	BinkMovie &operator=(const BinkMovie &) = delete;
	BinkMovie(BinkMovie &&) noexcept;
	BinkMovie &operator=(BinkMovie &&) noexcept;

	const BinkInfo &info() const;
	BinkStatus decode_next();
	const BinkFrame &frame() const;
	bool rewind();
	uint32_t frame_index() const;
	const std::string &last_error() const;

private:
	struct Impl;
	explicit BinkMovie(std::unique_ptr<Impl> impl);
	std::unique_ptr<Impl> impl_;
};

}  // namespace opennova::bink
