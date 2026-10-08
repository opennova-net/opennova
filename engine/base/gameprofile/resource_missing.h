#pragma once

#include <cstddef>
#include <string>
#include <utility>

namespace opennova::gameprofile {

// A name the runtime looked up as it loaded and did not find, said on one line of its log (ADR 0046
// DI-27): the marker, the kind of name, the name in quotes, the file that named it where the load
// site knows it, and what the game does without it:
//
//   <kResourceMissingMarker><kind> "<name>"[ named by "<by>"][: <words>]
//
// The kind is one of the words below: the namespaces the editor's asset graph resolves names in (its
// reference kinds' wire tokens), or "file" for a file the game opens by its own fixed name (hudpos.def,
// overcast.def, Avatars.def). A load site says each miss once a process (the Godot layer's
// ResourceRoot::report_missing), and the editor's Play reads each line back into a Problems row on the
// file that names the missing one (editor/session/play_log). Diagnostics only: nothing the game does
// reads them.
inline constexpr const char *kResourceMissingMarker = "resource missing: ";

namespace resource_kind {
inline constexpr const char *kFile = "file";                  // a file the game opens by its own name
inline constexpr const char *kModel = "model";                // a .3di an item's graphic names
inline constexpr const char *kTexture = "texture";            // a model material row's texture
inline constexpr const char *kSound = "sound";                // a sound set by name, in the banks loaded
inline constexpr const char *kSoundBank = "sound_bank";       // a .lwf by its file name
inline constexpr const char *kWave = "wave";                  // a .wav a sound bank's member names
inline constexpr const char *kAnimationMap = "animation_map"; // an .adm by its file name
inline constexpr const char *kEnvironment = "environment";    // a mission's .env
} // namespace resource_kind

// What one such line says.
struct ResourceMiss {
	std::string kind;  // one of resource_kind's words
	std::string name;  // as the load site looked it up
	std::string by;    // the file that named it, where the site knows it ("" for none)
	std::string words; // what the game does without it ("" for nothing said)
};

inline bool operator==(const ResourceMiss &a, const ResourceMiss &b) {
	return a.kind == b.kind && a.name == b.name && a.by == b.by && a.words == b.words;
}

namespace resource_missing_detail {
inline constexpr const char *kNamedBy = " named by \"";
inline std::string unquoted(const std::string &text) {
	std::string out;
	out.reserve(text.size());
	for (char c : text)
		if (c != '"' && c != '\r' && c != '\n') out.push_back(c);
	return out;
}
} // namespace resource_missing_detail

// The line's text from the marker on (the site puts its own prefix before it). A quote in the name or
// the file that named it, which no file name the game opens holds, is left out.
inline std::string resource_missing_text(const ResourceMiss &miss) {
	using namespace resource_missing_detail;
	std::string out = std::string(kResourceMissingMarker) + miss.kind + " \"" + unquoted(miss.name) + "\"";
	if (!miss.by.empty()) out += std::string(kNamedBy) + unquoted(miss.by) + "\"";
	if (!miss.words.empty()) out += ": " + miss.words;
	return out;
}

// The miss a line of the log says (the marker anywhere in it, a CR or LF at its end ignored); false for
// a line with no marker, or whose kind or quoted name is missing.
inline bool parse_resource_missing(const std::string &line, ResourceMiss &out) {
	const std::string marker = kResourceMissingMarker;
	const size_t at = line.find(marker);
	if (at == std::string::npos) return false;
	std::string text = line.substr(at + marker.size());
	while (!text.empty() && (text.back() == '\r' || text.back() == '\n')) text.pop_back();
	const size_t space = text.find(' ');
	if (space == std::string::npos || space == 0) return false;
	ResourceMiss miss;
	miss.kind = text.substr(0, space);
	if (space + 1 >= text.size() || text[space + 1] != '"') return false;
	const size_t name_end = text.find('"', space + 2);
	if (name_end == std::string::npos) return false;
	miss.name = text.substr(space + 2, name_end - space - 2);
	if (miss.name.empty()) return false;
	size_t rest = name_end + 1;
	const std::string named_by = resource_missing_detail::kNamedBy;
	if (text.compare(rest, named_by.size(), named_by) == 0) {
		const size_t by_start = rest + named_by.size();
		const size_t by_end = text.find('"', by_start);
		if (by_end == std::string::npos) return false;
		miss.by = text.substr(by_start, by_end - by_start);
		rest = by_end + 1;
	}
	if (text.compare(rest, 2, ": ") == 0) miss.words = text.substr(rest + 2);
	out = std::move(miss);
	return true;
}

} // namespace opennova::gameprofile
