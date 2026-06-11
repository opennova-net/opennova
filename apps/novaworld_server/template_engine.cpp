#include "template_engine.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace opennova::server {

namespace {

std::string trim(std::string_view s) {
	size_t b = 0, e = s.size();
	while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
	while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
	return std::string(s.substr(b, e - b));
}

} // namespace

std::string render_template(std::string_view tmpl, const TemplateVars &vars) {
	std::string out;
	out.reserve(tmpl.size());

	size_t pos = 0;
	while (pos < tmpl.size()) {
		const auto open = tmpl.find("{{", pos);
		if (open == std::string_view::npos) {
			out.append(tmpl.substr(pos));
			break;
		}
		out.append(tmpl.substr(pos, open - pos));

		const auto close = tmpl.find("}}", open + 2);
		if (close == std::string_view::npos) {
			// Unterminated `{{` — emit verbatim and stop scanning.
			out.append(tmpl.substr(open));
			break;
		}

		const auto key = trim(tmpl.substr(open + 2, close - open - 2));
		auto it = vars.find(key);
		if (it != vars.end()) {
			out.append(it->second);
		} else {
			// Leave unset markers in place so dev can spot them.
			out.append(tmpl.substr(open, close - open + 2));
		}
		pos = close + 2;
	}

	return out;
}

std::string render_template_file(const std::string &templates_dir,
                                 const std::string &filename,
                                 const TemplateVars &vars) {
	std::filesystem::path p = std::filesystem::path(templates_dir) / filename;
	std::ifstream in(p, std::ios::binary);
	if (!in) {
		return {};
	}
	std::ostringstream os;
	os << in.rdbuf();
	return render_template(os.str(), vars);
}

} // namespace opennova::server
