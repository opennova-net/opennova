#include "tpj/tpj_io.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>

#include <io/strutil.h>

namespace opennova {

namespace {

std::vector<std::string> tokenize(const std::string &line) {
	std::vector<std::string> tokens;
	bool in_quotes = false;
	std::string current;

	for (char c : line) {
		if (c == '"') {
			in_quotes = !in_quotes;
		} else if ((c == ' ' || c == '\t' || c == ',') && !in_quotes) {
			if (!current.empty()) {
				tokens.push_back(current);
				current.clear();
			}
		} else {
			current += c;
		}
	}
	if (!current.empty()) {
		tokens.push_back(current);
	}
	return tokens;
}

using opennova::strutil::iequals;

bool has_metadata_fields(const TpjProject &project) {
	return project.has_metadata
		|| !project.charmap.empty()
		|| !project.foliagemap.empty()
		|| !project.tilestrip.empty()
		|| !project.tileinfo.empty()
		|| !project.foliage_defs.empty();
}

void parse_foliage_attrib_tokens(const std::vector<std::string> &tokens, FoliageDef &def) {
	for (size_t i = 1; i < tokens.size(); ++i) {
		if (iequals(tokens[i], "forceon")) {
			def.attrib_flags = static_cast<uint8_t>(def.attrib_flags | FOLIAGE_ATTRIB_FORCE_ON);
		} else if (iequals(tokens[i], "shadow")) {
			def.attrib_flags = static_cast<uint8_t>(def.attrib_flags | FOLIAGE_ATTRIB_SHADOW);
		}
	}
}

void parse_foliage_block(std::istream &input, TpjProject &out) {
	if (out.foliage_defs.size() >= 4) {
		std::string discard;
		while (std::getline(input, discard)) {
			auto semi = discard.find(';');
			if (semi != std::string::npos) {
				discard = discard.substr(0, semi);
			}
			auto tokens = tokenize(discard);
			if (!tokens.empty() && iequals(tokens[0], "end")) {
				break;
			}
		}
		out.has_metadata = true;
		return;
	}

	FoliageDef def;
	std::string line;
	while (std::getline(input, line)) {
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		auto semi = line.find(';');
		if (semi != std::string::npos) {
			line = line.substr(0, semi);
		}
		auto tokens = tokenize(line);
		if (tokens.empty()) {
			continue;
		}
		if (tokens[0].size() > 0 && tokens[0][0] == '/') {
			continue;
		}
		if (iequals(tokens[0], "end")) {
			break;
		}
		if (tokens.size() < 2) {
			continue;
		}

		if (iequals(tokens[0], "graphic")) {
			def.graphic = tokens[1];
		} else if (iequals(tokens[0], "color_lower")) {
			def.color_lower = std::atoi(tokens[1].c_str());
		} else if (iequals(tokens[0], "color_upper")) {
			def.color_upper = std::atoi(tokens[1].c_str());
		} else if (iequals(tokens[0], "match")) {
			def.match = std::atoi(tokens[1].c_str());
		} else if (iequals(tokens[0], "attrib")) {
			parse_foliage_attrib_tokens(tokens, def);
		}
	}

	out.foliage_defs.push_back(foliage_normalize_def(def));
	out.has_metadata = true;
}

} // namespace

bool load_tpj(std::istream &input, TpjProject &out, std::string &error) {
	(void)error;
	out = TpjProject{};

	std::string line;
	while (std::getline(input, line)) {
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}

		auto semi = line.find(';');
		if (semi != std::string::npos) {
			line = line.substr(0, semi);
		}

		auto tokens = tokenize(line);
		if (tokens.empty()) {
			continue;
		}
		if (tokens[0][0] == '/') {
			continue;
		}

		const std::string &key = tokens[0];
		if (iequals(key, "foliage")) {
			parse_foliage_block(input, out);
			continue;
		}

		if (tokens.size() < 2) {
			continue;
		}

		if (iequals(key, "terrainname")) {
			out.terrain_name = tokens[1];
		} else if (iequals(key, "creator")) {
			out.creator = tokens[1];
		} else if (iequals(key, "path")) {
			out.path = tokens[1];
		} else if (iequals(key, "depthmap")) {
			out.depthmap = tokens[1];
		} else if (iequals(key, "output")) {
			out.output = tokens[1];
		} else if (iequals(key, "lock_topleft") && tokens.size() > 2) {
			out.lock_topleft = {std::atoi(tokens[1].c_str()), std::atoi(tokens[2].c_str())};
		} else if (iequals(key, "lock_topright") && tokens.size() > 2) {
			out.lock_topright = {std::atoi(tokens[1].c_str()), std::atoi(tokens[2].c_str())};
		} else if (iequals(key, "lock_bottomleft") && tokens.size() > 2) {
			out.lock_bottomleft = {std::atoi(tokens[1].c_str()), std::atoi(tokens[2].c_str())};
		} else if (iequals(key, "lock_bottomright") && tokens.size() > 2) {
			out.lock_bottomright = {std::atoi(tokens[1].c_str()), std::atoi(tokens[2].c_str())};
		} else if (iequals(key, "charmap")) {
			out.charmap = tokens[1];
			out.has_metadata = true;
		} else if (iequals(key, "foliagemap")) {
			out.foliagemap = tokens[1];
			out.has_metadata = true;
		} else if (iequals(key, "tilestrip")) {
			out.tilestrip = tokens[1];
			out.has_metadata = true;
		} else if (iequals(key, "tileinfo")) {
			out.tileinfo = tokens[1];
			out.has_metadata = true;
		}
	}

	if (out.depthmap.empty()) {
		error = "TPJ missing depthmap";
		return false;
	}

	return true;
}

bool save_tpj(std::ostream &output, const TpjProject &project, std::string &error) {
	const char *nl = "\r\n";

	output << "terrainname\t\"" << project.terrain_name << "\"" << nl;
	output << "creator\t\t\"" << project.creator << "\"" << nl;
	if (!project.path.empty()) {
		output << "path\t\t\"" << project.path << "\"" << nl;
	}
	output << "depthmap\t\"" << project.depthmap << "\"" << nl;
	if (!project.output.empty()) {
		output << "output\t\t\"" << project.output << "\"" << nl;
	}
	output << nl;
	output << "lock_topleft\t\t" << project.lock_topleft.x << "\t" << project.lock_topleft.y << nl;
	output << "lock_topright\t\t" << project.lock_topright.x << "\t" << project.lock_topright.y << nl;
	output << "lock_bottomleft\t\t" << project.lock_bottomleft.x << "\t" << project.lock_bottomleft.y << nl;
	output << "lock_bottomright\t" << project.lock_bottomright.x << "\t" << project.lock_bottomright.y << nl;

	if (has_metadata_fields(project)) {
		output << nl;
		if (!project.charmap.empty()) {
			output << "charmap\t\t\"" << project.charmap << "\"" << nl;
		}
		if (!project.foliagemap.empty()) {
			output << "foliagemap\t\"" << project.foliagemap << "\"" << nl;
		}
		if (!project.tilestrip.empty()) {
			output << "tilestrip\t\"" << project.tilestrip << "\"" << nl;
		}
		if (!project.tileinfo.empty()) {
			output << "tileinfo\t\"" << project.tileinfo << "\"" << nl;
		}

		for (const auto &def : project.foliage_defs) {
			const FoliageDef normalized = foliage_normalize_def(def);
			output << nl << "foliage" << nl;
			output << "  graphic\t\t\"" << normalized.graphic << "\"" << nl;
			output << "  color_lower\t\t" << normalized.color_lower << nl;
			output << "  color_upper\t\t" << normalized.color_upper << nl;
			output << "  match\t\t" << normalized.match << nl;
			if (normalized.attrib_flags != 0) {
				output << "  attrib\t\t";
				bool wrote = false;
				if ((normalized.attrib_flags & FOLIAGE_ATTRIB_SHADOW) != 0) {
					output << "shadow";
					wrote = true;
				}
				if ((normalized.attrib_flags & FOLIAGE_ATTRIB_FORCE_ON) != 0) {
					if (wrote) {
						output << " ";
					}
					output << "forceon";
				}
				output << nl;
			}
			output << "end" << nl;
		}
	}

	if (!output.good()) {
		error = "Write error for TPJ";
		return false;
	}
	return true;
}

TpjProject load_tpj_file(const std::string &filepath) {
	std::ifstream file(filepath);
	if (!file.is_open()) {
		throw std::runtime_error("Failed to open .tpj file: " + filepath);
	}

	TpjProject project;
	std::string error;
	if (!load_tpj(file, project, error)) {
		throw std::runtime_error(error.empty() ? "Failed to parse .tpj file: " + filepath : error);
	}

	return project;
}

void save_tpj_file(const std::string &filepath, const TpjProject &project) {
	std::ofstream file(filepath, std::ios::binary);
	if (!file.is_open()) {
		throw std::runtime_error("Failed to open .tpj for write: " + filepath);
	}

	std::string error;
	if (!save_tpj(file, project, error)) {
		throw std::runtime_error(error.empty() ? "Failed to write .tpj file: " + filepath : error);
	}
}

} // namespace opennova
