#include "credentials.h"

#include <base/io/os_path.h>
#include <base/io/strutil.h>

#include <fstream>

namespace opennova::serve {

bool load_credentials(const std::string &path, Credentials &out, std::string &error) {
	std::ifstream f(io::os_path(path), std::ios::binary);
	if (!f) {
		error = "cannot open the credentials file";
		return false;
	}
	Credentials c;
	std::string raw;
	while (std::getline(f, raw)) {
		const std::string line = strutil::trim(raw);
		if (line.empty() || line[0] == '#' || line[0] == ';')
			continue;
		const size_t eq = line.find('=');
		if (eq == std::string::npos)
			continue;
		const std::string key = strutil::trim(line.substr(0, eq));
		std::string value = strutil::trim(line.substr(eq + 1));
		if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') &&
				value.back() == value.front())
			value = value.substr(1, value.size() - 2);
		if (key == "NOVAWORLD_USER")
			c.user = value;
		else if (key == "NOVAWORLD_PASS")
			c.pass = value;
	}
	out = c;
	return true;
}

} // namespace opennova::serve
