#include "template_engine.h"

#include "auth.h"

#include <base/io/file_io.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <system_error>

namespace opennova::novaworld_server {

namespace {

std::string trim(std::string_view s) {
	size_t b = 0, e = s.size();
	while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
	while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
	return std::string(s.substr(b, e - b));
}

} // namespace

std::string html_escape(std::string_view text) {
	std::string out;
	out.reserve(text.size());
	for (const char c : text) {
		switch (c) {
			case '&':
				out += "&amp;";
				break;
			case '<':
				out += "&lt;";
				break;
			case '>':
				out += "&gt;";
				break;
			case '"':
				out += "&quot;";
				break;
			case '\'':
				out += "&#39;";
				break;
			default:
				out.push_back(c);
				break;
		}
	}
	return out;
}

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
			out.append(html_escape(it->second));
		} else {
			// Leave unset markers in place so dev can spot them.
			out.append(tmpl.substr(open, close - open + 2));
		}
		pos = close + 2;
	}

	return out;
}

bool is_bare_file_name(std::string_view name) {
	if (name.empty() || name.front() == '.')
		return false;
	return std::all_of(name.begin(), name.end(), [](char c) {
		return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
				c == '_' || c == '.' || c == '-';
	});
}

std::optional<std::filesystem::path> file_under_root(
		const std::filesystem::path &root, std::string_view relative) {
	namespace fs = std::filesystem;
	bool named = !relative.empty();
	for (size_t start = 0; named;) {
		const size_t slash = relative.find('/', start);
		named = is_bare_file_name(relative.substr(
				start, slash == std::string_view::npos ? std::string_view::npos : slash - start));
		if (slash == std::string_view::npos)
			break;
		start = slash + 1;
	}
	if (!named) {
		std::fprintf(stderr, "[http] refused a file name: '%s'\n", loggable(relative).c_str());
		return std::nullopt;
	}

	// The segment rule already keeps the path under the root lexically; the
	// canonical check also refuses a symlink under the root that leads out.
	std::error_code ec;
	fs::path base = fs::weakly_canonical(root, ec);
	if (ec)
		return std::nullopt;
	if (base.filename().empty())
		base = base.parent_path(); // a trailing separator
	const fs::path full = fs::weakly_canonical(base / fs::path(std::string(relative)), ec);
	if (ec)
		return std::nullopt;
	auto at = full.begin();
	for (const fs::path &part : base) {
		if (at == full.end() || *at != part) {
			std::fprintf(stderr, "[http] refused a file outside its root: '%s'\n",
					loggable(relative).c_str());
			return std::nullopt;
		}
		++at;
	}
	if (at == full.end() || !fs::is_regular_file(full, ec))
		return std::nullopt;
	return full;
}

std::optional<std::filesystem::path> template_file(
		const std::string &templates_dir, std::string_view name) {
	if (name.find('/') != std::string_view::npos) {
		std::fprintf(stderr, "[http] refused a template name: '%s'\n", loggable(name).c_str());
		return std::nullopt;
	}
	return file_under_root(templates_dir, name);
}

std::string render_template_file(const std::string &templates_dir,
                                 const std::string &filename,
                                 const TemplateVars &vars) {
	const auto path = template_file(templates_dir, filename);
	if (!path)
		return {};
	std::string text;
	std::string error;
	if (!io::read_file_text(path->string(), text, error))
		return {};
	return render_template(text, vars);
}

} // namespace opennova::novaworld_server
