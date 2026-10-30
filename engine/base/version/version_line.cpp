#include <base/version/version_line.h>

#include <utility>
#include <vector>

namespace opennova {

namespace {

// " · ": U+00B7 MIDDLE DOT in UTF-8, between two blanks.
constexpr const char *kSeparator = " \xC2\xB7 ";

} // namespace

std::string version_line(const Versions &versions) {
	std::vector<std::string> parts;
	if (!versions.game.empty()) {
		std::string game = versions.game;
		if (versions.game_version && versions.game_version->count > 0)
			game += " " + versions.game_version->text();
		parts.push_back(std::move(game));
	}
	parts.push_back(versions.one.label());
	if (!versions.protocol.empty())
		parts.push_back(protocol_words(versions.protocol, versions.protocol_version));
	std::string line;
	for (const std::string &part : parts) {
		if (!line.empty())
			line += kSeparator;
		line += part;
	}
	return line;
}

std::string protocol_words(std::string_view name, std::string_view version) {
	if (name.empty())
		return std::string();
	std::string words = std::string(name) + " protocol";
	if (!version.empty())
		words += " " + std::string(version);
	return words;
}

} // namespace opennova
