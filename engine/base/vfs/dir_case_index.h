#pragma once

// A loose directory's files by name without case: the one index every loose-file lookup outside
// the mount's own search paths finds a file by (the game's resource root, the texture and
// sidecar resolves, a font's folder). Retail finds a loose file by opening "<dir>\<name>" on a
// file system that matches a name in any case, so a name resolves whatever case the folder spells
// it in; the index stands in for that open on any file system. A case-sensitive file system can
// hold two names that differ only in case, which retail's cannot: such a name is ambiguous and
// resolves no file, so which one a lookup took never depends on the listing's order.
// [orig: FileSystem_FileExists @0x75AA50 - sprintf("%s\\%s") @0x75AAD4, _lopen @0x75AAE3]

#include <string>
#include <unordered_map>

namespace opennova {

class DirCaseIndex {
public:
	// The files of `dir` (UTF-8; sub-directories are not listed). A directory that does not open
	// gives the empty index.
	static DirCaseIndex scan(const std::string &dir);

	// The name `name` matches on disk, as the folder spells it; "" when the folder holds none, or
	// holds it in two cases (ambiguous).
	std::string find(const std::string &name) const;
	// The folder holds `name` in two or more cases.
	bool ambiguous(const std::string &name) const;
	bool empty() const { return names_.empty(); }

private:
	// The ASCII-lowered name -> the name on disk, "" for an ambiguous name.
	std::unordered_map<std::string, std::string> names_;
};

} // namespace opennova
