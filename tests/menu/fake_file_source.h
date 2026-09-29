#pragma once

// A FileSource over named byte strings with stamps, counting every read: the menu input
// tests' stand-in for the game's mounted root and the editor's project files.

#include <base/io/strutil.h>
#include <base/vfs/file_source.h>

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

class FakeFileSource : public opennova::FileSource {
public:
	void put(const std::string &name, std::vector<uint8_t> bytes, uint64_t stamp) {
		files_[opennova::strutil::to_lower(name)] = {std::move(bytes), stamp};
	}
	void put_text(const std::string &name, const std::string &text, uint64_t stamp) {
		put(name, std::vector<uint8_t>(text.begin(), text.end()), stamp);
	}
	void remove(const std::string &name) { files_.erase(opennova::strutil::to_lower(name)); }
	bool read(const std::string &name, std::vector<uint8_t> &out) const override {
		++reads[opennova::strutil::to_lower(name)];
		const auto found = files_.find(opennova::strutil::to_lower(name));
		if (found == files_.end()) return false;
		out = found->second.first;
		return true;
	}
	uint64_t stamp(const std::string &name) const override {
		const auto found = files_.find(opennova::strutil::to_lower(name));
		return found == files_.end() ? 0 : found->second.second;
	}
	int total_reads() const {
		int total = 0;
		for (const auto &entry : reads) total += entry.second;
		return total;
	}
	// Reads by lowercased name since the last clear.
	mutable std::map<std::string, int> reads;

private:
	std::map<std::string, std::pair<std::vector<uint8_t>, uint64_t>> files_;
};
