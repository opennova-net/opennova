// The in-memory mission::BootFileSource the ungated mission-kernel, runtime-boot,
// host-role, tick-digest and WAC surface tests boot over: a name -> bytes map
// standing in for the mounted resource root. Header-only, infrastructure only
// (no retail counterpart to cite).
#pragma once

#include <runtime/mission/runtime_boot.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace test_boot {

// A source over a BORROWED map: `files` must outlive the source and every
// kernel opened over it. A name the map does not carry reports absent and
// reads false with `out` untouched; names match exactly (no case folding).
inline opennova::mission::BootFileSource source_over(
		const std::map<std::string, std::string> *files) {
	opennova::mission::BootFileSource s;
	s.has_file = [files](const std::string &name) {
		return files->find(name) != files->end();
	};
	s.read_file = [files](const std::string &name, std::vector<uint8_t> &out) {
		const auto it = files->find(name);
		if (it == files->end()) return false;
		out.assign(it->second.begin(), it->second.end());
		return true;
	};
	return s;
}

} // namespace test_boot
