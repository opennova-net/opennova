#include "machine_identity.h"

#include <base/io/file_io.h>
#include <base/io/json.h>
#include <base/io/strutil.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace opennova::hostd {

bool parse_machine_id(std::string_view text, int64_t &out) {
	if (text.empty() || text.size() > 15 || !strutil::all_digits(text))
		return false;
	const auto id = strutil::parse_llong(std::string(text));
	if (!id || *id <= 0)
		return false;
	out = *id;
	return true;
}

bool read_machine_file(const std::string &path, int64_t &out, std::string &error) {
	std::string text;
	if (!io::read_file_text(path, text, error)) {
		error = "the machine id file " + path + " does not read: " + error;
		return false;
	}
	if (!parse_machine_id(strutil::trim(text), out)) {
		error = "the machine id file " + path + " holds no machine id";
		return false;
	}
	return true;
}

bool ecs_config_machine_id(std::string_view config, std::string &out, std::string &error) {
	static constexpr std::string_view kKey = "ECS_INSTANCE_ATTRIBUTES=";
	std::string attributes;
	bool found = false;
	size_t start = 0;
	while (start < config.size()) {
		size_t end = config.find('\n', start);
		if (end == std::string_view::npos)
			end = config.size();
		const std::string line = strutil::trim(config.substr(start, end - start));
		start = end + 1;
		if (line.rfind(kKey, 0) == 0) {
			attributes = line.substr(kKey.size());
			found = true;
		}
	}
	if (!found) {
		error = "it sets no ECS_INSTANCE_ATTRIBUTES";
		return false;
	}
	io::JsonValue v;
	std::string why;
	if (!io::json_parse(attributes, v, why) || !v.is_object()) {
		error = "its ECS_INSTANCE_ATTRIBUTES is no JSON object";
		return false;
	}
	const io::JsonValue *id = v.get("machine-id");
	if (!id || !id->is_string() || id->string.empty()) {
		error = "its ECS_INSTANCE_ATTRIBUTES names no machine-id";
		return false;
	}
	out = id->string;
	return true;
}

std::string alive_path(const std::string &state_dir) {
	return state_dir + "/alive";
}

bool touch_alive(const std::string &path) {
	const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0)
		return false;
	const bool ok = ::futimens(fd, nullptr) == 0;
	::close(fd);
	return ok;
}

bool alive_within(const std::string &path, int64_t max_age_s, std::time_t now) {
	struct stat st{};
	if (::stat(path.c_str(), &st) != 0)
		return false;
	return now - st.st_mtime <= max_age_s;
}

} // namespace opennova::hostd
