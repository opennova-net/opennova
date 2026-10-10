#include <editor/graph/texture_import_needs.h>

#include <set>

#include <base/io/strutil.h>
#include <editor/documents/texture_roles.h>
#include <editor/import/texture_import.h>
#include <editor/project/project_files.h>

namespace opennova::editor {

namespace {

using R = renderer::TextureRoleId;

// What one use asks of an option, and why.
struct Ask {
	std::string value;
	std::string why;
	const TextureUse *use = nullptr;
};

bool model_row(R role) { return role == R::ModelDiffuse || role == R::ModelDetail || role == R::ModelFlipFrame; }

std::string extension_of(const std::string &name) { return strutil::to_lower(utf8_of(path_of(name).extension())); }
std::string stem_of(const std::string &name) { return utf8_of(path_of(basename_of(name)).stem()); }

// The format a use asks of its file: the one its name's extension says, a .tga by its role; a sky's cloud
// layer from a source with an alpha, the .dds its loader reads first.
std::string format_for(const TextureUse &use, bool source_alpha, std::string &why) {
	const std::string extension = extension_of(use.name_written);
	const std::string name = basename_of(use.name_written);
	if (use.role == R::SkyCloud && source_alpha) {
		// [orig: Texture_LoadFromArchive @ 0x58BA1B..0x58BA50: the .dds beside the name before it, its alpha
		//  as it is; a PCX's alpha is its palette's luminance @ 0x58BC21..0x58BCFB]
		why = "its loader reads " + stem_of(name) + ".dds before " + name +
		      ", and only a DDS keeps the source's alpha as the cloud's density (a PCX's is its colours' brightness)";
		return "dds";
	}
	if (extension == ".pcx" || extension == ".png" || extension == ".mdt" || extension == ".dds") {
		why = "it names " + name;
		return extension.substr(1);
	}
	if (extension != ".tga") return std::string();
	if (model_row(use.role)) {
		// [orig: Texture_LoadByNameWithChannel @ 0x58B53C..0x58B5C0: the .dds beside the name first]
		why = "its loader reads " + stem_of(name) + ".dds before " + name +
		      ", and the game's own model textures are DXT5 with their mips";
		return "dds";
	}
	if (use.role == R::TerrainColourMap) {
		// [orig: PolyTrn_InitTextures @ 0x60B5A9..0x60B6FD: the alpha premultiplied for the far terrain]
		why = "a colour map's alpha darkens the far terrain, so it holds none";
		return "tga24";
	}
	why = "it names " + name;
	return "tga";
}

std::set<std::string> values_of(const std::vector<Ask> &asks) {
	std::set<std::string> out;
	for (const Ask &ask : asks) out.insert(strutil::to_lower(ask.value));
	return out;
}

// A conflict in words: each use and what it asks.
std::string conflict(const std::string &option, const std::vector<Ask> &asks) {
	std::string words = option + ":";
	for (const Ask &ask : asks) words += " " + ask.value + " for " + ask.use->words + ";";
	words.back() = '.';
	return words + " No one file serves them all: make one file for each.";
}

// The referring files whose uses ask otherwise than the first use's (a split's), once each; none where one
// of them is also a file the first's value is asked by (a file a split could not part).
void split_of(TextureImportNeeds &out, const std::vector<Ask> &asks) {
	if (!out.split_referrers.empty() || asks.empty()) return;
	const std::string first = strutil::to_lower(asks.front().value);
	std::set<std::string> keeping, moving;
	for (const Ask &ask : asks) {
		if (ask.use->fixed || ask.use->referrer.empty()) return;
		(strutil::to_lower(ask.value) == first ? keeping : moving).insert(ask.use->referrer);
	}
	for (const std::string &file : moving)
		if (keeping.count(file)) return;
	out.split_referrers.assign(moving.begin(), moving.end());
}

// The option set, with the reason of the first use that asked it.
void choose(TextureImportNeeds &out, const std::string &option, const std::string &value, const std::vector<Ask> &asks) {
	out.options[option] = value;
	for (const Ask &ask : asks)
		if (strutil::to_lower(ask.value) == strutil::to_lower(value)) {
			out.reasons.push_back(option + " " + value + ": " + ask.use->words + " (" + ask.why + ")");
			return;
		}
	out.reasons.push_back(option + " " + value + ": what serves every use");
}

} // namespace

TextureImportNeeds texture_import_needs(const std::vector<TextureUse> &uses, const std::string &source_name, bool source_alpha) {
	TextureImportNeeds out;
	std::vector<Ask> formats, stems, sizes, palettes, normals;
	const bool indexed_source = extension_of(source_name) == ".pcx";
	for (const TextureUse &use : uses) {
		++out.uses;
		std::string why;
		std::string format = format_for(use, source_alpha, why);
		if (use.role == R::TerrainFoliageMap || use.role == R::TerrainCharMap) {
			// Its palette indices are data the game reads: kept as they are from an 8-bit PCX source.
			if (!indexed_source) {
				out.conflicts.push_back(use.words + ": its palette indices are data the game reads, which an import from "
				                                    "colours cannot keep. Make it an 8-bit PCX in a paint program, and import that.");
				continue;
			}
			format = "pcx";
			why = "its palette indices are data the game reads";
			palettes.push_back({"indices", why + ", kept from the 8-bit PCX source", &use});
		}
		if (use.role == R::ModelHeightNormal)
			// [orig: Texture_LoadAsNormalMap @0x58C985..0x58CAED: the alpha's height made into the normal map]
			normals.push_back({"height", "the game makes the normal map from the height in its alpha", &use});
		if (format.empty()) continue;
		formats.push_back({format, why, &use});
		stems.push_back({stem_of(use.name_written), "it names " + basename_of(use.name_written), &use});
		if (!use.known()) continue;
		const TextureRoleRow &role = texture_role_row(use.role);
		if (role.size == renderer::TextureSizeRule::Exact)
			sizes.push_back({std::to_string(role.width) + "x" + std::to_string(role.height),
			                 "the game reads it at " + texture_size_words(role), &use});
		else if (role.size == renderer::TextureSizeRule::PowerOfTwo)
			sizes.push_back({"pow2_down", "the game asks " + texture_size_words(role), &use});
	}
	if (formats.empty()) return out;
	// The format: one asked, or among TGA and DDS asks a TGA, which serves every one (a model row reads a
	// .tga when no .dds is there; a colour map reads a 32-bit TGA's colour).
	const std::set<std::string> asked = values_of(formats);
	std::string format;
	if (asked.size() == 1) format = *asked.begin();
	else if (!asked.count("pcx") && !asked.count("png") && !asked.count("mdt"))
		format = asked.count("tga") ? "tga" : asked.count("tga24") ? "tga24" : "dds";
	if (format.empty()) {
		out.conflicts.push_back(conflict("format", formats));
		split_of(out, formats);
		return out;
	}
	choose(out, "format", format, formats);
	// The name: the stem every use writes, with the format's extension; none needed where that is the
	// source's own stem.
	if (values_of(stems).size() > 1) {
		out.conflicts.push_back(conflict("name", stems));
	} else {
		const std::string name = stems.front().value + renderer::image_format_extension(format);
		if (strutil::to_lower(name) != strutil::to_lower(stem_of(source_name) + renderer::image_format_extension(format)))
			choose(out, "name", name, {{name, stems.front().why, stems.front().use}});
	}
	if (!sizes.empty()) {
		if (values_of(sizes).size() == 1) choose(out, "size", sizes.front().value, sizes);
		else {
			out.conflicts.push_back(conflict("size", sizes));
			split_of(out, sizes);
		}
	}
	if (format == "pcx" && !palettes.empty()) choose(out, "palette", "indices", palettes);
	if (format == "tga" && !normals.empty()) {
		// A use that reads the colour as it is (the HUD, a model's diffuse) and one that reads a height in
		// the alpha do not share a file.
		if (normals.size() == formats.size()) choose(out, "normal", "height", normals);
		else out.conflicts.push_back("normal: height for " + normals.front().use->words +
		                             ", and its colour as it is for the other uses. No one file serves them all: make "
		                             "one file for each.");
	}
	return out;
}

} // namespace opennova::editor
