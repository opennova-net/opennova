#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace opennova {

// Whole files by flat logical name (the basename, compared without case, the way the
// mounted file set is looked up): the game's mounted resource root, or an embedder's own
// set (the editor's project files, with the documents it has open standing in for
// theirs). read() answers the decoded payload; stamp() changes whenever read() of that
// name may answer differently, and is 0 when the name does not resolve. A holder keys
// what it parsed or decoded on (name, stamp), so an unchanged stamp is a reuse and a
// moved one a reload.
class FileSource {
public:
	virtual ~FileSource() = default;
	virtual bool read(const std::string &name, std::vector<uint8_t> &out) const = 0;
	virtual uint64_t stamp(const std::string &name) const = 0;
};

} // namespace opennova
