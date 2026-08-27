#include <formats/bink/bink.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

class BitWriter {
public:
	void write(uint32_t value, unsigned count) {
		for (unsigned bit = 0; bit < count; ++bit) {
			if ((bit_count_ & 7) == 0) {
				bytes_.push_back(0);
			}
			bytes_.back() |= ((value >> bit) & 1U) << (bit_count_ & 7);
			++bit_count_;
		}
	}

	void align32() {
		while ((bit_count_ & 31) != 0) {
			write(0, 1);
		}
	}

	const std::vector<uint8_t> &bytes() const { return bytes_; }

private:
	std::vector<uint8_t> bytes_;
	unsigned bit_count_ = 0;
};

void append_le32(std::vector<uint8_t> &bytes, uint32_t value) {
	bytes.push_back(static_cast<uint8_t>(value));
	bytes.push_back(static_cast<uint8_t>(value >> 8));
	bytes.push_back(static_cast<uint8_t>(value >> 16));
	bytes.push_back(static_cast<uint8_t>(value >> 24));
}

void write_fill_plane(BitWriter &writer, uint8_t color) {
	// Seven identity trees, plus the sixteen high-nibble context trees used
	// by the color bundle.
	writer.write(0, 4);  // block types
	writer.write(0, 4);  // scaled sub-types
	for (unsigned i = 0; i < 16; ++i) {
		writer.write(0, 4);
	}
	writer.write(0, 4);  // color low nibble
	writer.write(0, 4);  // patterns
	writer.write(0, 4);  // X motion
	writer.write(0, 4);  // Y motion
	writer.write(0, 4);  // runs

	writer.write(1, 10);  // one block type
	writer.write(1, 1);   // repeat encoding
	writer.write(6, 4);   // fill block
	writer.write(0, 9);   // no scaled sub-types
	writer.write(1, 10);  // one color
	writer.write(1, 1);   // repeat encoding
	writer.write(color >> 4, 4);
	writer.write(color & 0xf, 4);
	writer.write(0, 10);  // patterns
	writer.write(0, 10);  // X motion
	writer.write(0, 10);  // Y motion
	writer.write(0, 10);  // intra DC
	writer.write(0, 10);  // inter DC
	writer.write(0, 10);  // runs
	writer.align32();
}

std::vector<uint8_t> make_white_biki() {
	BitWriter packet;
	packet.write(0, 32);
	write_fill_plane(packet, 235);  // Y
	write_fill_plane(packet, 128);  // V
	write_fill_plane(packet, 128);  // U

	const uint32_t first_frame_offset = 48;
	const uint32_t file_size = first_frame_offset +
			static_cast<uint32_t>(packet.bytes().size());
	std::vector<uint8_t> file;
	file.reserve(file_size);
	append_le32(file, 0x694b4942U);  // BIKi
	append_le32(file, file_size - 8);
	append_le32(file, 1);  // frames
	append_le32(file, static_cast<uint32_t>(packet.bytes().size()));
	append_le32(file, 0);  // reserved
	append_le32(file, 8);
	append_le32(file, 8);
	append_le32(file, 30);
	append_le32(file, 1);
	append_le32(file, 0);  // flags
	append_le32(file, 0);  // audio tracks
	append_le32(file, first_frame_offset | 1U);
	file.insert(file.end(), packet.bytes().begin(), packet.bytes().end());
	return file;
}

std::unique_ptr<opennova::bink::BinkMovie> open_memory(
		const std::shared_ptr<std::vector<uint8_t>> &bytes, std::string &error) {
	opennova::bink::BinkSource source;
	source.size = bytes->size();
	source.read_at = [bytes](uint64_t offset, uint8_t *destination, size_t size) {
		if (offset > bytes->size() || size > bytes->size() - offset) {
			return false;
		}
		std::memcpy(destination, bytes->data() + offset, size);
		return true;
	};
	return opennova::bink::BinkMovie::open(std::move(source), &error);
}

bool expect(bool condition, const char *message) {
	if (!condition) {
		std::cerr << "FAIL: " << message << '\n';
	}
	return condition;
}

}  // namespace

int main() {
	int failures = 0;
	auto bytes = std::make_shared<std::vector<uint8_t>>(make_white_biki());
	std::string error;
	auto movie = open_memory(bytes, error);
	if (!expect(movie != nullptr, error.c_str())) {
		return 1;
	}
	const auto &info = movie->info();
	failures += !expect(info.width == 8 && info.height == 8,
			"synthetic dimensions were not parsed");
	failures += !expect(info.frame_count == 1 && info.frames_per_second() == 30.0,
			"synthetic timing was not parsed");
	failures += !expect(movie->frame_index() == 0,
			"a new movie must point at frame zero");
	failures += !expect(movie->decode_next() == opennova::bink::BinkStatus::frame_ready,
			movie->last_error().c_str());
	failures += !expect(movie->frame_index() == 1,
			"decoding must advance the frame index");
	const auto &frame = movie->frame();
	failures += !expect(frame.width == 8 && frame.height == 8 && frame.stride == 32,
			"decoded RGBA geometry is wrong");
	failures += !expect(frame.rgba.size() == 8 * 8 * 4,
			"decoded RGBA storage is wrong");
	// Full-scale luma (Y 235, the clamp row's 219) lands on 254 under retail's
	// truncating 38154/32768 ramp (binkw32 YUV_init @ 0x30019F00), not 255.
	for (size_t pixel = 0; pixel < 64; ++pixel) {
		const uint8_t *rgba = frame.rgba.data() + pixel * 4;
		if (rgba[0] != 254 || rgba[1] != 254 || rgba[2] != 254 || rgba[3] != 255) {
			++failures;
			std::cerr << "FAIL: fill frame pixel " << pixel << " is not retail white (254)\n";
			break;
		}
	}
	// The witnessed YUV -> RGB law (bink.cpp yuv_to_rgb): truncating
	// fixed-point ramps, per-channel clamp. Values computed from the retail
	// table formulas by hand.
	{
		struct Pin { uint8_t y, u, v, r, g, b; };
		const Pin pins[] = {
			{16, 128, 128, 0, 0, 0},       // black
			{235, 128, 128, 254, 254, 254}, // retail white
			{128, 128, 128, 130, 130, 130}, // mid grey: 112 * 38154 / 32768 = 130
			{128, 90, 240, 255, 53, 54},    // R clamps, G 130 - 91 + 14, B 130 - 76
			{60, 200, 50, 0, 86, 196},
			{0, 0, 0, 0, 154, 0},           // YUV zero is not black
			{255, 255, 255, 255, 102, 255},
			{100, 128, 140, 116, 88, 97},
			{200, 64, 64, 112, 255, 85},
		};
		for (const Pin &p : pins) {
			uint8_t r = 0, g = 0, b = 0;
			opennova::bink::yuv_to_rgb(p.y, p.u, p.v, r, g, b);
			if (r != p.r || g != p.g || b != p.b) {
				++failures;
				std::cerr << "FAIL: yuv_to_rgb(" << int(p.y) << "," << int(p.u) << "," << int(p.v)
						  << ") = " << int(r) << "," << int(g) << "," << int(b) << " expected "
						  << int(p.r) << "," << int(p.g) << "," << int(p.b) << "\n";
			}
		}
	}
	failures += !expect(movie->decode_next() == opennova::bink::BinkStatus::end_of_stream,
			"the one-frame stream must end");
	failures += !expect(movie->rewind() && movie->frame_index() == 0,
			"rewind must reset sequential decode");
	failures += !expect(movie->decode_next() == opennova::bink::BinkStatus::frame_ready,
			"the rewound frame must decode again");

	(*bytes)[0] = 'N';
	auto invalid_magic = open_memory(bytes, error);
	failures += !expect(invalid_magic == nullptr && error.find("BIKi") != std::string::npos,
			"non-BIKi input must be rejected");
	(*bytes)[0] = 'B';
	(*bytes)[40] = 1;
	auto audio = open_memory(bytes, error);
	failures += !expect(audio == nullptr && error.find("audio") != std::string::npos,
			"audio-bearing Bink input must be explicitly rejected");

	if (failures == 0) {
		std::cout << "Bink container and fill-block decode tests passed\n";
	}
	return failures == 0 ? 0 : 1;
}
