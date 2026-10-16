#include "object_store.h"

#include <base/io/strutil.h>

namespace opennova::novaworld_server {

bool parse_object_store_url(std::string_view url, std::string_view region, ObjectStoreConfig &out,
		std::string &error) {
	ObjectStoreConfig config;
	if (url.rfind("s3://", 0) == 0) {
		std::string_view rest = url.substr(5);
		const size_t slash = rest.find('/');
		if (slash == std::string_view::npos || slash == 0) {
			error = "an S3 store URL is s3://<bucket>/<root>";
			return false;
		}
		config.kind = ObjectStoreConfig::Kind::S3;
		config.bucket = std::string(rest.substr(0, slash));
		std::string_view root = rest.substr(slash + 1);
		while (!root.empty() && root.back() == '/')
			root.remove_suffix(1);
		if (root.empty() || !is_store_key(root)) {
			error = "an S3 store URL needs a root under the bucket (s3://<bucket>/<root>)";
			return false;
		}
		config.root = std::string(root);
		if (region.empty()) {
			error = "an S3 store needs its region (ONNET_OBJECT_STORE_REGION)";
			return false;
		}
		config.region = std::string(region);
	} else if (url.rfind("file://", 0) == 0) {
		std::string_view dir = url.substr(7);
		if (dir.size() > 2 && dir[0] == '/' && dir[2] == ':')
			dir.remove_prefix(1); // file:///C:/dir
		while (dir.size() > 1 && dir.back() == '/')
			dir.remove_suffix(1);
		if (dir.empty()) {
			error = "a local store URL is file:///<directory>";
			return false;
		}
		config.kind = ObjectStoreConfig::Kind::Local;
		config.root = std::string(dir);
	} else {
		error = "the object store URL is s3://<bucket>/<root> or file:///<directory>";
		return false;
	}
	out = std::move(config);
	return true;
}

bool is_store_key(std::string_view key) {
	if (key.empty() || key.size() > 512 || key.front() == '/' || key.back() == '/')
		return false;
	size_t part_start = 0;
	for (size_t i = 0; i <= key.size(); ++i) {
		if (i == key.size() || key[i] == '/') {
			const std::string_view part = key.substr(part_start, i - part_start);
			if (part.empty() || part == "." || part == "..")
				return false;
			part_start = i + 1;
			continue;
		}
		const char c = key[i];
		const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
				(c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
		if (!ok)
			return false;
	}
	return true;
}

} // namespace opennova::novaworld_server
