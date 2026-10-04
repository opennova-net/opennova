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

bool decodes(const std::string &name, const std::vector<uint8_t> &bytes, bool &indexed, std::string &why) {
	ImageSource source;
	if (!decode_image_source(name, bytes, source, why)) return false;
	indexed = source.indexed;
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
		out["format"] = "pcx";
		if (indexed_source) out["palette"] = "indices";
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
                                       const ImportOptions &overrides) {
	TextureSourcePlan plan;
	const std::string image = basename_of(image_name);
	const std::string extension = extension_of(image);
	bool indexed = false;
	std::string why;
	if (extension != ".png" && extension != ".tga" && extension != ".pcx") {
		plan.refusals.push_back(refused(image + " is no image the importer reads: a PNG, a TGA or a PCX.", texture));
		return plan;
	}
	if (!decodes(image, image_bytes, indexed, why)) {
		plan.refusals.push_back(refused(image + " does not read: " + why + ".", texture));
		return plan;
	}
	const AssetEntry *entry = scan.at_path(texture);
	if (!entry) entry = scan.find(basename_of(texture));
	std::string kept_name;
	ImportOptions options;
	if (entry && !entry->imported_from.empty()) {
		// An import's output: its import's own options, its source set aside where the new one is another file.
		plan.texture = entry->logical_name;
		plan.old_source = entry->imported_from;
		ImportSidecar record;
		Diagnostic error;
		if (load_import_sidecar(join_path(paths.root, plan.old_source + kImportSidecarSuffix), record, error)) options = record.options;
		options.erase("name");
	} else if (entry && entry->kind == AssetKind::Texture) {
		plan.texture = entry->logical_name;
		plan.replaced = entry->relative_path;
		std::vector<uint8_t> bytes;
		std::string message;
		read_file_bytes(join_path(paths.root, entry->relative_path), bytes, message);
		options = texture_reproducing_options(entry->logical_name, bytes, image, indexed);
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
		options = texture_reproducing_options(plan.texture, {}, image, indexed);
		options.erase("name");
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
	if (!entry->imported_from.empty()) {
		plan.source = entry->imported_from; // its own source, as it is
		return plan;
	}
	if (entry->kind == AssetKind::ImportSource) {
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
	const std::shared_ptr<const TextureImage> image = decode_texture(entry->logical_name, bytes);
	if (!image || !image->loads || !image->decoded || image->levels.empty()) {
		plan.refusals.push_back(refused(entry->logical_name + " does not read, so no image of it can be made" +
		                                        (image && !image->refusal.empty() ? ": " + image->refusal : std::string()) + ".",
		                                entry->relative_path));
		return plan;
	}
	plan.replaced = entry->relative_path;
	std::string wanted;
	bool indexed = false;
	if (extension == ".tga" || extension == ".mdt" || extension == ".pcx") {
		// A copy a paint program opens: a TGA's bytes (an .mdt's are a TGA's), a PCX's (its indices kept).
		wanted = stem_of(entry->logical_name) + (extension == ".pcx" ? ".pcx" : ".tga");
		plan.bytes = bytes;
		indexed = extension == ".pcx" && !image->indices.empty();
	} else {
		// A DDS's first level as its reader decodes it, a PNG.
		wanted = stem_of(entry->logical_name) + ".png";
		const TextureLevel &level = image->levels.front();
		plan.bytes = encode_png_rgba(level.rgba.data(), level.width, level.height);
	}
	const std::string name = free_source_name(scan, wanted, plan.texture, std::string());
	if (name.empty() || plan.bytes.empty()) {
		plan.refusals.push_back(refused("No source could be made for " + entry->logical_name + ".", entry->relative_path));
		return plan;
	}
	plan.source = "art/" + name;
	plan.options = texture_reproducing_options(entry->logical_name, bytes, name, indexed);
	return plan;
}

bool apply_texture_source(const ProjectPaths &paths, const TextureSourcePlan &plan, std::vector<Diagnostic> &findings) {
	if (!plan.ok()) {
		findings.insert(findings.end(), plan.refusals.begin(), plan.refusals.end());
		return false;
	}
	if (plan.bytes.empty()) return true; // the source is there as it is
	const auto at = [&](const std::string &relative) { return system_path(join_path(paths.root, relative)); };
	// Set aside, never deleted: .opennova/replaced/<stamp>/<the file's own path>.
	const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
	char stamp[32];
	std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", std::localtime(&now));
	const std::string aside = join_path(".opennova/replaced", stamp);
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
	const auto put_back = [&] {
		std::error_code ec;
		for (auto it = moved.rbegin(); it != moved.rend(); ++it) rename_with_retry(it->second, it->first, ec);
	};
	bool ok = set_aside(plan.replaced) && set_aside(plan.old_source) &&
	          set_aside(plan.old_source.empty() ? std::string() : plan.old_source + kImportSidecarSuffix);
	// The source's own old bytes where it is written over (Replace of an output by an image of its kind).
	if (ok && plan.old_source.empty() && plan.replaced.empty()) {
		std::error_code ec;
		if (fs::exists(at(plan.source), ec)) {
			const fs::path to = at(join_path(aside, plan.source));
			fs::create_directories(to.parent_path(), ec);
			fs::copy_file(at(plan.source), to, fs::copy_options::overwrite_existing, ec);
		}
	}
	std::string message;
	if (ok) {
		std::error_code ec;
		fs::create_directories(at(plan.source).parent_path(), ec);
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
