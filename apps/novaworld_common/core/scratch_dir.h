#pragma once

// An owner-only scratch directory under the system's temporary one: what a
// program's AWS CLI, curl and SES calls read and write (the object store's,
// billing's, the mail's, the machine driver's), each removed with its owner.
// Service policy, not a port.

#include <filesystem>
#include <string>

namespace opennova::novaworld_server {

// An owner-only scratch directory under the system's temporary one, removed
// with its owner; a move hands it on (the source then owns none).
class ScratchDir {
public:
	ScratchDir() = default;
	ScratchDir(ScratchDir &&other) noexcept;
	ScratchDir &operator=(ScratchDir &&other) noexcept;
	ScratchDir(const ScratchDir &) = delete;
	ScratchDir &operator=(const ScratchDir &) = delete;
	~ScratchDir();

	// A new one named `prefix` and a random tail (random_secret.h); empty
	// with `error` when it cannot be made (owner_file.h make_owner_directory).
	static ScratchDir make(const char *prefix, std::string &error);

	const std::filesystem::path &path() const { return path_; }
	bool empty() const { return path_.empty(); }

private:
	void remove();
	std::filesystem::path path_;
};

} // namespace opennova::novaworld_server
