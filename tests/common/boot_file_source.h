// The in-memory mission::BootFileSource the ungated mission-kernel, runtime-boot,
// host-role, tick-digest and WAC surface tests boot over: a name -> bytes map
// standing in for the mounted resource root, plus the bare no-net tick those
// tests drive a booted kernel with. Header-only, infrastructure only (no retail
// counterpart to cite).
#pragma once

#include <runtime/inmatch/local_role.h>
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

// The bare no-net tick: the local role over the kernel (ADR 0043 d3; the
// kernel itself owns no tick).
inline void tick_no_net(opennova::mission::MissionKernel &kernel) {
	opennova::inmatch::LocalRole role;
	role.bind(kernel);
	role.run_tick(opennova::inmatch::TickInput{});
}

} // namespace test_boot
