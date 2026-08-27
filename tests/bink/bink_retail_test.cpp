#include <formats/bink/bink.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

uint64_t fnv1a(const std::vector<uint8_t> &bytes) {
	uint64_t hash = 1469598103934665603ULL;
	for (uint8_t byte : bytes) {
		hash ^= byte;
		hash *= 1099511628211ULL;
	}
	return hash;
}

}  // namespace

int main() {
	const char *root = std::getenv("OPENNOVA_JO_DIR");
	if (root == nullptr || *root == '\0') {
		std::cout << "SKIP: set OPENNOVA_JO_DIR to decode the retail main.bik\n";
		return 0;
	}
	const std::filesystem::path path = std::filesystem::path(root) / "main.bik";
	auto file = std::make_shared<std::ifstream>(path, std::ios::binary);
	if (!*file) {
		std::cerr << "FAIL: cannot open " << path << '\n';
		return 1;
	}
	file->seekg(0, std::ios::end);
	const uint64_t size = static_cast<uint64_t>(file->tellg());
	file->seekg(0, std::ios::beg);
	opennova::bink::BinkSource source;
	source.size = size;
	source.read_at = [file](uint64_t offset, uint8_t *destination, size_t count) {
		file->clear();
		file->seekg(static_cast<std::streamoff>(offset), std::ios::beg);
		file->read(reinterpret_cast<char *>(destination),
				static_cast<std::streamsize>(count));
		return file->good() || static_cast<size_t>(file->gcount()) == count;
	};
	std::string error;
	auto movie = opennova::bink::BinkMovie::open(std::move(source), &error);
	if (!movie) {
		std::cerr << "FAIL: " << error << '\n';
		return 1;
	}
	if (movie->info().width != 800 || movie->info().height != 450 ||
			movie->info().frame_count != 1856) {
		std::cerr << "FAIL: unexpected retail main.bik metadata\n";
		return 1;
	}
	uint64_t first_hash = 0;
	bool changed = false;
	for (unsigned index = 0; index < 120; ++index) {
		if (movie->decode_next() != opennova::bink::BinkStatus::frame_ready) {
			std::cerr << "FAIL: frame " << index << ": " << movie->last_error() << '\n';
			return 1;
		}
		const auto &frame = movie->frame();
		const uint64_t hash = fnv1a(frame.rgba);
		if (index == 0) {
			first_hash = hash;
		} else {
			changed = changed || hash != first_hash;
		}
		if (hash == 0 || frame.rgba.size() != 800U * 450U * 4U) {
			std::cerr << "FAIL: retail frame storage is invalid\n";
			return 1;
		}
		for (size_t alpha = 3; alpha < frame.rgba.size(); alpha += 4) {
			if (frame.rgba[alpha] != 255) {
				std::cerr << "FAIL: decoded frame has non-opaque pixels\n";
				return 1;
			}
		}
		if (index % 30 == 0) {
			std::cout << "frame " << index << " rgba-fnv1a=" << std::hex << hash <<
					std::dec << '\n';
		}
	}
	if (!changed) {
		std::cerr << "FAIL: the first 120 retail frames never change\n";
		return 1;
	}
	return 0;
}
