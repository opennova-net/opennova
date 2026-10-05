#include <editor/import/texture_source.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <system_error>

#include <base/io/strutil.h>
#include <editor/documents/texture_image.h>
#include <editor/import/importer.h>
#include <editor/import/png_encode.h>
#include <editor/import/sidecar.h>
#include <editor/import/texture_import.h>
#include <editor/project/project_document.h>
#include <formats/pff/pff.h>

namespace fs = std::filesystem;

namespace opennova::editor {

namespace {

std::string extension_of(const std::string &name) { return strutil::to_lower(utf8_of(path_of(name).extension())); }
std::string stem_of(const std::string &name) { return utf8_of(path_of(basename_of(name)).stem()); }

Diagnostic refused(const std::string &message, const std::string &asset) {
	return make_finding(CoreFinding::TextureReplace, DiagnosticSeverity::Error, message, asset);
}

// A file name for the source in art/ that no file of the project has and that fits an archive's 16
// characters (an import source's name binds as its outputs' would): `wanted` where it is free and not the
// texture's own name (a source named like its output would be two files of one name), else its stem with
// "_src" and a number, cut to fit.
std::string free_source_name(const AssetScan &scan, const std::string &wanted, const std::string &texture,
                             const std::string &allowed_path) {
	const std::string extension = utf8_of(path_of(wanted).extension());
	const std::string stem = stem_of(wanted);
	const auto free = [&](const std::string &name) {
		if (name.size() > size_t(pff::PFF_NAME_SIZE) || normalized_logical_name(name) == normalized_logical_name(texture))
			return false;
		const AssetEntry *taken = scan.find(name);
		return !taken || taken->relative_path == allowed_path;
	};
	if (free(wanted)) return wanted;
	for (int n = 1; n < 100; ++n) {
		const std::string suffix = n == 1 ? "_src" : "_src" + std::to_string(n);
		const size_t room = size_t(pff::PFF_NAME_SIZE) - suffix.size() - extension.size();
		const std::string name = stem.substr(0, std::min(stem.size(), room)) + suffix + extension;
		if (free(name)) return name;
	}
	return std::string();
}

// A texture's sides and stored form in words ("256 x 128, a 32-bit TGA"); "" for bytes that say nothing.
std::string texture_words(const std::string &name, const std::vector<uint8_t> &bytes) {
	const TextureHeader header = texture_header(name, bytes);
	if (!header.read) return std::string();
	std::string form;
	switch (header.reader) {
	case TextureReader::Tga:
		form = extension_of(name) == ".mdt" ? "a TGA under .mdt" : header.tga_bits == 32 ? "a 32-bit TGA" : "a " + std::to_string(header.tga_bits) + "-bit TGA";
		break;
	case TextureReader::Pcx: form = header.pcx_rgb() ? "a 24-bit PCX" : "an 8-bit PCX"; break;
	case TextureReader::Dds:
		form = "a " + (header.dds_format.empty() ? std::string("DDS") : header.dds_format + " DDS") +
		       (header.dds_levels > 1 ? " of " + std::to_string(header.dds_levels) + " levels" : std::string());
		break;
	case TextureReader::Png: form = "a PNG"; break;
	case TextureReader::None: break;
	}
	return std::to_string(header.width) + " x " + std::to_string(header.height) + (form.empty() ? "" : ", " + form);
}

// The texture the import makes of the plan's source (its bytes), and what the import said of it; false,
// with the findings, where it makes none.
bool make_output(const ProjectPaths &paths, TextureSourcePlan &plan, const std::string &source_name) {
	ImportContext context(source_name, plan.bytes, plan.options, paths.root, paths.root);
	ImportProduct product;
	if (!run_image_import(context, product) || product.outputs.empty()) {
		for (const Diagnostic &d : product.diagnostics)
			if (d.severity == DiagnosticSeverity::Error) plan.refusals.push_back(refused(d.message, plan.texture));
		if (plan.refusals.empty()) plan.refusals.push_back(refused("The import makes nothing of " + source_name + ".", plan.texture));
		return false;
	}
	plan.made = std::move(product.outputs.front().bytes);
	plan.made_name = product.outputs.front().name;
	plan.after_words = texture_words(plan.made_name, plan.made);
	for (const Diagnostic &d : product.diagnostics)
		if (d.severity != DiagnosticSeverity::Error) plan.changes.push_back(d.message);
	return true;
}

// A cube map or a volume, refused: the editor writes flat textures.
bool layered(TextureSourcePlan &plan, const std::string &name, const std::vector<uint8_t> &bytes, const std::string &asset) {
	const TextureHeader header = texture_header(name, bytes);
	if (!header.layered()) return false;
	plan.refusals.push_back(refused(basename_of(name) +
	                                        (header.dds_faces > 1
	                                                 ? " is a cube map of six faces, and the editor makes a flat texture of one, which "
	                                                   "the cube map's loader could not take. Edit it in a program that writes cube maps."
	                                                 : " is a volume of " + std::to_string(header.dds_depth) +
	                                                           " slices, and the editor makes a flat texture of one. Edit it in a "
	                                                           "program that writes volume textures."),
	                                asset));
	return true;
}

} // namespace

ImportOptions texture_reproducing_options(const std::string &name, const std::vector<uint8_t> &bytes,
                                          const std::string &source_name, bool indexed_source) {
	ImportOptions out;
	const std::string extension = extension_of(name);
	const TextureHeader header = bytes.empty() ? TextureHeader() : texture_header(name, bytes);
	if (extension == ".tga") {
		out["format"] = header.read && !header.alpha ? "tga24" : "tga";
	} else if (extension == ".dds") {
		out["format"] = "dds";
		if (header.dds_format == "DXT1") out["dds"] = "dxt1";
		else if (header.dds_format == "A8R8G8B8") out["dds"] = "argb";
		// A DXT file of one level stays one (an A8R8G8B8 is always one).
		if (header.read && header.dds_levels <= 1 && header.dds_format != "A8R8G8B8") out["mips"] = "none";
	} else if (extension == ".pcx") {
		// Its three planes of colour stay so; its 8-bit form's indices are kept from an indexed source.
		out["format"] = header.pcx_rgb() ? "pcx24" : "pcx";
		if (indexed_source && !header.pcx_rgb()) out["palette"] = "indices";
	} else if (extension == ".mdt") {
		out["format"] = "mdt";
	} else if (extension == ".png") {
		out["format"] = "png";
	}
	const auto format = out.find("format");
	const std::string made = stem_of(source_name) + image_format_extension(format == out.end() ? "tga" : format->second);
	if (normalized_logical_name(made) != normalized_logical_name(basename_of(name))) out["name"] = basename_of(name);
	return out;
}

TextureSourcePlan plan_texture_replace(const ProjectPaths &paths, const AssetScan &scan, const std::string &texture,
                                       const std::string &image_name, const std::vector<uint8_t> &image_bytes,
                                       const ImportOptions &overrides, const TextureUseAsks &asks) {
	TextureSourcePlan plan;
	const std::string image = basename_of(image_name);
	const std::string extension = extension_of(image);
	if (extension != ".png" && extension != ".tga" && extension != ".pcx") {
		plan.refusals.push_back(refused(image + " is no image the importer reads: a PNG, a TGA or a PCX.", texture));
		return plan;
	}
	ImageSource decoded;
	std::string why;
	if (!decode_image_source(image, image_bytes, decoded, why)) {
		plan.refusals.push_back(refused(image + " does not read: " + why + ".", texture));
		return plan;
	}
	const AssetEntry *entry = scan.at_path(texture);
	if (!entry) entry = scan.find(basename_of(texture));
	ImportOptions options;
	std::vector<uint8_t> current;
	std::string current_name;
	if (entry && !entry->imported_from.empty() && entry->kind != AssetKind::Texture) {
		plan.refusals.push_back(refused(entry->logical_name + " is no texture: an image cannot make it.", entry->relative_path));
		return plan;
	}
	if (entry && !entry->imported_from.empty()) {
		// An import's output: its import's own options, its source set aside where the new one is another file.
		plan.texture = entry->logical_name;
		plan.old_source = entry->imported_from;
		ImportSidecar record;
		Diagnostic error;
		if (load_import_sidecar(join_path(paths.root, plan.old_source + kImportSidecarSuffix), record, error)) options = record.options;
		options.erase("name");
		std::string message;
		read_file_bytes(join_path(paths.root, entry->relative_path), current, message);
		current_name = entry->logical_name;
		plan.changes.push_back(plan.texture + " is made as its import makes it now (" + plan.old_source + "'s options), from " + image + ".");
	} else if (entry && entry->kind == AssetKind::Texture) {
		plan.texture = entry->logical_name;
		plan.replaced = entry->relative_path;
		std::string message;
		read_file_bytes(join_path(paths.root, entry->relative_path), current, message);
		current_name = entry->logical_name;
		options = texture_reproducing_options(entry->logical_name, current, image, decoded.indexed);
		options.erase("name");
	} else if (entry) {
		plan.refusals.push_back(refused(entry->logical_name + " is no texture: " + entry->relative_path +
		                                        (entry->kind == AssetKind::ImportSource ? " is an import's source, the image it reads."
		                                                                                : " is of another kind."),
		                                entry->relative_path));
		return plan;
	} else {
		// A name the project lacks (a field's missing texture): made under that name.
		plan.texture = basename_of(texture);
		FileNameProblem problem = FileNameProblem::None;
		std::string message;
		if (!check_file_name(plan.texture, AssetKind::Texture, problem, message)) {
			plan.refusals.push_back(refused(message, plan.texture));
			return plan;
		}
		options = texture_reproducing_options(plan.texture, {}, image, decoded.indexed);
		options.erase("name");
		plan.changes.push_back(plan.texture + ", which the project lacks, is made from " + image + ".");
	}
	if (!current.empty() && layered(plan, current_name, current, plan.texture)) return plan;
	plan.before_words = current.empty() ? std::string() : texture_words(current_name, current);
	// What the uses ask: their palette indices from an indexed image alone; their exact size.
	if (asks.indices && !decoded.indexed) {
		plan.refusals.push_back(refused(plan.texture + "'s uses read its palette indices (" + asks.indices_why + "), which " + image +
		                                        ", an image of colours, cannot carry: bring an 8-bit PCX of the codes.",
		                                plan.texture));
		return plan;
	}
	if (asks.indices && extension_of(plan.texture) == ".pcx") options["palette"] = "indices";
	const std::string wanted = std::to_string(decoded.image.width) + "x" + std::to_string(decoded.image.height);
	if (!asks.size.empty() && strutil::to_lower(asks.size) != wanted && !overrides.count("size") && !options.count("size")) {
		options["size"] = asks.size;
		plan.changes.push_back(image + " is " + std::to_string(decoded.image.width) + " x " + std::to_string(decoded.image.height) +
		                       ": resized to " + asks.size + ", as " + asks.size_why + ".");
	}
	for (const auto &[key, value] : overrides) {
		const ImportOptionRow *row = import_option_row(image_import_option_rows(), key);
		const std::string taken = row && row->keeps_case ? value : strutil::to_lower(value);
		if (!row || (!taken.empty() && !import_option_accepts(*row, taken))) {
			plan.refusals.push_back(refused("The image importer takes no " + key + " '" + value + "'.", plan.texture));
			return plan;
		}
		if (taken.empty()) options.erase(key);
		else options[key] = taken;
	}
	// The source: the old source's own path where the image is of its kind, else a free name in art/.
	if (!plan.old_source.empty() && extension_of(plan.old_source) == extension) {
		plan.source = plan.old_source;
		plan.old_source.clear();
		plan.changes.push_back(plan.source + " is written over with " + image + " (its old bytes kept in " + kReplacedFolder + "/).");
	} else {
		const std::string name = free_source_name(scan, image, plan.texture, std::string());
		if (name.empty()) {
			plan.refusals.push_back(refused("No free name in art/ for " + image + ".", plan.texture));
			return plan;
		}
		plan.source = "art/" + name;
	}
	plan.bytes = image_bytes;
	// The output takes the texture's name.
	const auto format = options.find("format");
	const std::string made = stem_of(plan.source) + image_format_extension(format == options.end() ? "tga" : format->second);
	if (normalized_logical_name(made) != normalized_logical_name(plan.texture)) options["name"] = plan.texture;
	else options.erase("name");
	plan.options = std::move(options);
	if (!make_output(paths, plan, basename_of(plan.source))) return plan;
	if (!plan.replaced.empty())
		plan.changes.push_back(plan.replaced + " is set aside in " + std::string(kReplacedFolder) +
		                       "/, never deleted; " + plan.source + " makes " + plan.texture + " from now on.");
	if (!plan.old_source.empty())
		plan.changes.push_back(plan.old_source + ", the source it was made from, is set aside in " + std::string(kReplacedFolder) + "/.");
	return plan;
}

TextureSourcePlan plan_texture_source(const ProjectPaths &paths, const AssetScan &scan, const std::string &texture) {
	TextureSourcePlan plan;
	const AssetEntry *entry = scan.at_path(texture);
	if (!entry) entry = scan.find(basename_of(texture));
	if (!entry) {
		plan.refusals.push_back(refused("The project has no texture named '" + texture + "'.", texture));
		return plan;
	}
	plan.texture = entry->logical_name;
	// A texture's source: one the image importer reads (an .o3d's is a model's, which no paint program edits).
	const auto image_source = [&](const std::string &source, const std::string &asset) {
		ImportSidecar record;
		Diagnostic error;
		if (load_import_sidecar(join_path(paths.root, source + kImportSidecarSuffix), record, error) && record.importer == "image")
			return true;
		plan.refusals.push_back(refused(basename_of(source) + " is no texture's source: its import is " +
		                                        (record.importer.empty() ? std::string("none it reads") : record.importer + "'s") + ".",
		                                asset));
		return false;
	};
	if (!entry->imported_from.empty()) {
		if (entry->kind != AssetKind::Texture) {
			plan.refusals.push_back(refused(entry->logical_name + " is no texture: its program is not a paint program.", entry->relative_path));
			return plan;
		}
		if (!image_source(entry->imported_from, entry->relative_path)) return plan;
		plan.source = entry->imported_from; // its own source, as it is
		return plan;
	}
	if (entry->kind == AssetKind::ImportSource) {
		if (!image_source(entry->relative_path, entry->relative_path)) return plan;
		plan.source = entry->relative_path;
		return plan;
	}
	if (entry->kind != AssetKind::Texture) {
		plan.refusals.push_back(refused(entry->logical_name + " is no texture.", entry->relative_path));
		return plan;
	}
	const std::string extension = extension_of(entry->logical_name);
	if (extension == ".png") {
		plan.source = entry->relative_path; // the game reads it as it is: edited in place
		return plan;
	}
	std::vector<uint8_t> bytes;
	std::string message;
	if (!read_file_bytes(join_path(paths.root, entry->relative_path), bytes, message)) {
		plan.refusals.push_back(refused(message, entry->relative_path));
		return plan;
	}
	if (layered(plan, entry->logical_name, bytes, entry->relative_path)) return plan;
	const std::shared_ptr<const TextureImage> image = decode_texture(entry->logical_name, bytes);
	if (!image || !image->loads || !image->decoded || image->levels.empty()) {
		plan.refusals.push_back(refused(entry->logical_name + " does not read, so no image of it can be made" +
		                                        (image && !image->refusal.empty() ? ": " + image->refusal : std::string()) + ".",
		                                entry->relative_path));
		return plan;
	}
	plan.replaced = entry->relative_path;
	plan.before_words = texture_words(entry->logical_name, bytes);
	const TextureHeader header = texture_header(entry->logical_name, bytes);
	std::string wanted;
	bool indexed = false;
	if (extension == ".tga" || extension == ".mdt" || extension == ".pcx") {
		// A copy a paint program opens: a TGA's bytes (an .mdt's are a TGA's), a PCX's (its indices kept).
		wanted = stem_of(entry->logical_name) + (extension == ".pcx" ? ".pcx" : ".tga");
		plan.bytes = bytes;
		indexed = extension == ".pcx" && !image->indices.empty();
		if (header.reader == TextureReader::Tga && (header.tga_descriptor & 0x20))
			plan.changes.push_back(entry->logical_name + "'s rows are stored top first, so the game draws it upside down now; made from "
			                                             "its source, it is stored bottom first and drawn as its program shows it.");
	} else {
		// A DDS's first level as its reader decodes it, a PNG.
		wanted = stem_of(entry->logical_name) + ".png";
		const TextureLevel &level = image->levels.front();
		plan.bytes = encode_png_rgba(level.rgba.data(), level.width, level.height);
		plan.changes.push_back(entry->logical_name + " is made again from a PNG of its first level: its " +
		                       (header.dds_format.empty() ? std::string("DDS") : header.dds_format) + " texels are encoded again" +
		                       (header.dds_levels > 1 ? " and its " + std::to_string(header.dds_levels) + " levels made anew from the first"
		                                              : std::string()) +
		                       ", so the file the game reads changes before any edit.");
	}
	const std::string name = free_source_name(scan, wanted, plan.texture, std::string());
	if (name.empty() || plan.bytes.empty()) {
		plan.refusals.push_back(refused("No source could be made for " + entry->logical_name + ".", entry->relative_path));
		return plan;
	}
	plan.source = "art/" + name;
	plan.options = texture_reproducing_options(entry->logical_name, bytes, name, indexed);
	if (!make_output(paths, plan, name)) return plan;
	plan.changes.push_back(plan.source + ", a copy for its program, makes " + plan.texture + " from now on; " + plan.replaced +
	                       " is set aside in " + std::string(kReplacedFolder) + "/, never deleted.");
	return plan;
}

TextureSourcePlan plan_texture_dds(const ProjectPaths &paths, const AssetScan &scan, const std::string &texture,
                                   const std::vector<std::string> &reads_tga) {
	TextureSourcePlan plan;
	const auto refuse = [&](const std::string &message, const std::string &asset) {
		plan.refusals.push_back(make_finding(CoreFinding::TextureStoreDds, DiagnosticSeverity::Error, message, asset));
		return plan;
	};
	const AssetEntry *entry = scan.at_path(texture);
	if (!entry) entry = scan.find(basename_of(texture));
	if (!entry) return refuse("The project has no texture named '" + texture + "'.", texture);
	plan.texture = entry->logical_name;
	if (entry->kind != AssetKind::Texture) return refuse(entry->logical_name + " is no texture.", entry->relative_path);
	if (extension_of(entry->logical_name) != ".tga")
		return refuse(entry->logical_name + " is no .tga: only a .tga has a .dds its loaders read before it.", entry->relative_path);
	const std::string dds = stem_of(entry->logical_name) + ".dds";
	if (!reads_tga.empty())
		return refuse(reads_tga.front() + " reads " + entry->logical_name + " itself, never " + dds + ": stored as a DDS, the texture would be lost there" +
		                      (reads_tga.size() > 1 ? " (and " + std::to_string(reads_tga.size() - 1) + " more)." : std::string(".")),
		              entry->relative_path);
	const AssetEntry *taken = scan.find(dds);
	if (taken && taken->relative_path != entry->relative_path)
		return refuse("The project has " + dds + " already (" + taken->relative_path + "), which the game reads for " + entry->logical_name +
		                      " now: rename or remove it first.",
		              entry->relative_path);
	std::vector<uint8_t> bytes;
	std::string message;
	if (!read_file_bytes(join_path(paths.root, entry->relative_path), bytes, message)) return refuse(message, entry->relative_path);
	const std::shared_ptr<const TextureImage> image = decode_texture(entry->logical_name, bytes);
	if (!image || !image->loads || !image->decoded || image->levels.empty())
		return refuse(entry->logical_name + " does not read" + (image && !image->refusal.empty() ? ": " + image->refusal : std::string()) + ".",
		              entry->relative_path);
	const TextureLevel &level = image->levels.front();
	const auto power_of_two = [](uint32_t side) { return side != 0 && (side & (side - 1)) == 0; };
	if (!power_of_two(level.width) || !power_of_two(level.height))
		return refuse(entry->logical_name + " is " + std::to_string(level.width) + " x " + std::to_string(level.height) +
		                      ": the game pads a .dds whose sides are not powers of two to the next ones, so a model's UVs would "
		                      "reach the padding. Make each side a power of two first.",
		              entry->relative_path);
	bool opaque = true;
	for (size_t i = 3; i < level.rgba.size() && opaque; i += 4) opaque = level.rgba[i] == 255;
	plan.before_words = texture_words(entry->logical_name, bytes);
	ImportOptions options;
	options["format"] = "dds";
	options["dds"] = opaque ? "dxt1" : "dxt5";
	if (!entry->imported_from.empty()) {
		// An import's output: its own record takes the form (the session sets the options).
		ImportSidecar record;
		Diagnostic error;
		if (!load_import_sidecar(join_path(paths.root, entry->imported_from + kImportSidecarSuffix), record, error) ||
		    record.importer != "image")
			return refuse(entry->logical_name + " is made by an import that does not write a DDS.", entry->relative_path);
		plan.source = entry->imported_from;
		options["name"] = dds;
		plan.options = std::move(options);
		plan.changes.push_back(plan.source + "'s import writes " + dds + " in place of " + entry->logical_name + ".");
		return plan;
	}
	// A plain file: its source made once, a copy of the TGA in art/ (as Edit externally makes it), the plain file
	// set aside. Its own name where nothing else holds it, the .dds its output: the plain file it replaces leaves
	// the name free.
	const std::string name = free_source_name(scan, entry->logical_name, dds, entry->relative_path);
	if (name.empty()) return refuse("No source could be made for " + entry->logical_name + ".", entry->relative_path);
	plan.replaced = entry->relative_path;
	plan.bytes = std::move(bytes);
	plan.source = "art/" + name;
	if (normalized_logical_name(stem_of(name) + ".dds") != normalized_logical_name(dds)) options["name"] = dds;
	plan.options = std::move(options);
	if (!make_output(paths, plan, name)) return plan;
	plan.changes.push_back(entry->logical_name + " is stored as " + dds + " (" + plan.after_words + "), which every use of it reads first: "
	                       "its referrers keep naming " + entry->logical_name + ".");
	plan.changes.push_back(plan.source + ", a copy of it, makes " + dds + " from now on; " + plan.replaced + " is set aside in " +
	                       std::string(kReplacedFolder) + "/, never deleted.");
	return plan;
}

bool apply_texture_source(const ProjectPaths &paths, const TextureSourcePlan &plan, std::vector<Diagnostic> &findings) {
	if (!plan.ok()) {
		findings.insert(findings.end(), plan.refusals.begin(), plan.refusals.end());
		return false;
	}
	if (plan.bytes.empty()) return true; // the source is there as it is
	const auto at = [&](const std::string &relative) { return system_path(join_path(paths.root, relative)); };
	// Set aside, never deleted: .replaced/<stamp>/<the file's own path>, the stamp to the millisecond and
	// numbered past a folder already there (two set-asides within one never share it).
	const auto now = std::chrono::system_clock::now();
	const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
	const int millis = int(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
	char stamp[48];
	std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", std::localtime(&seconds));
	char milli[8];
	std::snprintf(milli, sizeof(milli), "%03d", millis);
	const std::string base = std::string(stamp) + "-" + milli;
	std::string folder = base;
	{
		std::error_code ec;
		for (int n = 2; fs::exists(at(join_path(kReplacedFolder, folder)), ec) && n < 1000; ++n)
			folder = base + "-" + std::to_string(n);
	}
	const std::string aside = join_path(kReplacedFolder, folder);
	std::vector<std::pair<fs::path, fs::path>> moved;
	const auto set_aside = [&](const std::string &relative) {
		std::error_code ec;
		if (relative.empty() || !fs::exists(at(relative), ec)) return true;
		const fs::path to = at(join_path(aside, relative));
		fs::create_directories(to.parent_path(), ec);
		if (!ec && rename_with_retry(at(relative), to, ec)) {
			moved.emplace_back(at(relative), to);
			return true;
		}
		findings.push_back(refused("Could not set " + relative + " aside: " + ec.message() + ".", relative));
		return false;
	};
	// A source written over: its old bytes kept beside the set-asides, and put back should the plan fail; a
	// source written new taken away again.
	std::vector<uint8_t> overwritten;
	bool had_source = false, wrote_source = false;
	const auto put_back = [&] {
		std::error_code ec;
		std::string message;
		if (wrote_source) {
			if (had_source) write_file_atomic(join_path(paths.root, plan.source), overwritten.data(), overwritten.size(), message);
			else fs::remove(at(plan.source), ec);
		}
		for (auto it = moved.rbegin(); it != moved.rend(); ++it) rename_with_retry(it->second, it->first, ec);
	};
	bool ok = set_aside(plan.replaced) && set_aside(plan.old_source) &&
	          set_aside(plan.old_source.empty() ? std::string() : plan.old_source + kImportSidecarSuffix);
	if (ok) {
		std::error_code ec;
		std::string message;
		if (fs::exists(at(plan.source), ec) && read_file_bytes(join_path(paths.root, plan.source), overwritten, message)) {
			had_source = true;
			const fs::path to = at(join_path(aside, plan.source));
			fs::create_directories(to.parent_path(), ec);
			fs::copy_file(at(plan.source), to, fs::copy_options::overwrite_existing, ec);
		}
	}
	std::string message;
	if (ok) {
		std::error_code ec;
		fs::create_directories(at(plan.source).parent_path(), ec);
		wrote_source = true;
		ok = write_file_atomic(join_path(paths.root, plan.source), plan.bytes.data(), plan.bytes.size(), message);
		if (!ok) findings.push_back(refused("Could not write " + plan.source + ": " + message + ".", plan.source));
	}
	if (ok) {
		// Its record: the image importer's, the options; the import pass fills in the rest.
		ImportSidecar record;
		record.importer = "image";
		record.version = kImageImporterVersion;
		record.options = plan.options;
		Diagnostic error;
		ok = save_import_sidecar(join_path(paths.root, plan.source + kImportSidecarSuffix), record, error);
		if (!ok) findings.push_back(error);
	}
	if (!ok) put_back();
	return ok;
}

} // namespace opennova::editor
