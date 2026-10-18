#include "scratch_dir.h"

#include "owner_file.h"
#include "random_secret.h"

#include <system_error>
#include <utility>

namespace opennova::novaworld_server {

ScratchDir::ScratchDir(ScratchDir &&other) noexcept : path_(std::exchange(other.path_, {})) {}

ScratchDir &ScratchDir::operator=(ScratchDir &&other) noexcept {
	if (this != &other) {
		remove();
		path_ = std::exchange(other.path_, {});
	}
	return *this;
}

ScratchDir::~ScratchDir() {
	remove();
}

void ScratchDir::remove() {
	if (path_.empty())
		return;
	std::error_code ec;
	std::filesystem::remove_all(path_, ec);
	path_.clear();
}

ScratchDir ScratchDir::make(const char *prefix, std::string &error) {
	ScratchDir dir;
	std::error_code ec;
	const std::filesystem::path temp = std::filesystem::temp_directory_path(ec);
	if (ec) {
		error = "no temporary directory";
		return dir;
	}
	const std::filesystem::path path = temp / (std::string(prefix) + random_secret(12));
	if (make_owner_directory(path, error))
		dir.path_ = path;
	return dir;
}

} // namespace opennova::novaworld_server
