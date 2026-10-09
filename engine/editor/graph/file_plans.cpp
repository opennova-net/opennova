#include <editor/graph/file_plans.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>
#include <system_error>

#include <base/io/strutil.h>
#include <editor/assets/asset_kinds.h>
#include <editor/assets/asset_type_registry.h>
#include <editor/assets/project_layout.h>
#include <editor/documents/mission_file_set.h>
#include <editor/import/import_run.h>
#include <editor/import/importer.h>
#include <editor/import/sidecar.h>
#include <editor/model/finding_code_row.h>
#include <editor/project/project_files.h>

namespace opennova::editor {

namespace fs = std::filesystem;

namespace {

Diagnostic refusal(CoreFinding code, const std::string &message, const std::string &asset = std::string()) {
	return make_finding(code, DiagnosticSeverity::Error, message, asset);
}

bool on_disk(const ProjectPaths &paths, const std::string &relative) {
	std::error_code ec;
	return fs::exists(system_path(join_path(paths.root, relative)), ec) || ec;
}

// A name's stem and its extension with its dot ("oncrate1", ".3di"); a name with no dot is all stem.
std::pair<std::string, std::string> split_name(const std::string &name) {
	const size_t dot = name.rfind('.');
	if (dot == std::string::npos || dot == 0) return {name, std::string()};
	return {name.substr(0, dot), name.substr(dot)};
}

// Whether `path` lies in the folder `folder` (project-relative, compared as the file system compares names).
bool in_folder(const std::string &path, const std::string &folder) {
	return path.size() > folder.size() && path[folder.size()] == '/' &&
	       strutil::iequals(path.substr(0, folder.size()), folder);
}

// The project's import record of the source at `relative`, read; false for none.
bool record_of(const ProjectPaths &paths, const std::string &relative, ImportSidecar &out) {
	Diagnostic unread;
	return load_import_sidecar(join_path(paths.root, relative + kImportSidecarSuffix), out, unread);
}

// Whether the copy named `name` of `file` is free: no file of the project has it nor any file its set would take
// (a mission's companions under its base name, a copied import source's outputs), the base game serves none of
// them, and nothing sits at the copy's place on disk. `with_record`: an import source copied with its record.
bool copy_name_free(const ProjectPaths *paths, const AssetScan &scan, const AssetEntry &file, const std::string &name,
                    bool with_record, const BaseNames *base, std::string *why = nullptr) {
	const auto taken = [&](const std::string &candidate, const char *as) {
		if (!scan.find(candidate) && !(base && base->has(candidate))) return false;
		if (why) *why = std::string(as) + candidate + (scan.find(candidate) ? " is a file of the project already." : " is the base game's.");
		return true;
	};
	if (taken(name, "")) return false;
	if (paths && on_disk(*paths, join_path(folder_of_path(file.relative_path), name))) {
		if (why) *why = "Something sits at " + join_path(folder_of_path(file.relative_path), name) + " already.";
		return false;
	}
	if (file.kind == AssetKind::Mission) {
		for (const MissionFileSetMember &member : mission_file_set_members(scan, file.logical_name, name))
			if (taken(member.new_name, "The copy's companion ")) return false;
		// A file the game would take as the copy's own (its text table) that is no copy of the mission's set.
		if (!mission_file_set_members(scan, name, name).empty()) {
			if (why) *why = "The project has files the game would take as " + name + "'s own.";
			return false;
		}
	}
	if (with_record)
		for (const AssetEntry &output : scan.entries) {
			if (output.imported_from != file.relative_path) continue;
			const std::string renamed = renamed_import_output(output.logical_name, file.logical_name, name);
			// An output its import names whatever the source is called follows no name: plan_duplicate refuses it.
			if (strutil::iequals(renamed, output.logical_name)) continue;
			if (!pff::logical_name_fits_archive(renamed)) {
				if (why) *why = "The copy's import output " + renamed + " would not fit the game's archives.";
				return false;
			}
			if (taken(renamed, "The copy's import output ")) return false;
		}
	return true;
}

} // namespace

std::vector<std::string> DeletePlan::named() const {
	std::vector<std::string> out;
	for (const std::string &file : files)
		if (!strutil::ends_with_icase(file, kImportSidecarSuffix)) out.push_back(file);
	if (!alone) out.insert(out.end(), outputs.begin(), outputs.end());
	return out;
}

DeletePlan plan_delete(const ProjectPaths &paths, const AssetScan &scan, const std::string &file, bool alone) {
	DeletePlan plan;
	const AssetEntry *asset = scan.named(file);
	if (!asset) {
		plan.refusals.push_back(refusal(CoreFinding::FileUnknown, "The project has no file named '" + file + "'.", file));
		return plan;
	}
	plan.path = asset->relative_path;
	plan.name = asset->logical_name;
	if (!asset->imported_from.empty()) {
		plan.refusals.push_back(refusal(CoreFinding::FileImported,
		                                asset->logical_name + " is made by the import of " + asset->imported_from +
		                                        ", which makes it again: delete the source.",
		                                asset->relative_path));
		return plan;
	}
	plan.files.push_back(asset->relative_path);
	// A mission takes the files the game finds by its name (ADR 0046 S14), as a rename takes them: left behind,
	// they would be another mission's of the same name.
	if (asset->kind == AssetKind::Mission)
		for (const MissionFileSetMember &member : mission_file_set_members(scan, asset->logical_name, asset->logical_name)) {
			const AssetEntry *entry = scan.at_path(member.path);
			if (entry && entry->imported_from.empty() && entry != asset) plan.files.push_back(entry->relative_path);
		}
	// An import source takes its record, and its outputs with it, or (alone) leaves them as files of the project.
	if (asset->kind == AssetKind::ImportSource) {
		const std::string record = asset->relative_path + kImportSidecarSuffix;
		if (on_disk(paths, record)) plan.files.push_back(record);
		plan.alone = alone;
		const std::string dir = import_output_dir(paths, asset->relative_path);
		for (const AssetEntry &output : scan.entries) {
			if (output.imported_from != asset->relative_path) continue;
			if (!alone) {
				plan.outputs.push_back(output.relative_path);
				continue;
			}
			// Kept where the placement rule puts a file of its kind, as an import would have put it.
			const std::string to = placement_path(scan, output.logical_name, output.kind);
			if (on_disk(paths, to)) {
				plan.refusals.push_back(refusal(CoreFinding::FileExists,
				                                "Something sits at " + to + " already, where " + output.logical_name +
				                                        " would be kept.",
				                                asset->relative_path));
				continue;
			}
			plan.kept.emplace_back(output.relative_path, to);
		}
		if (on_disk(paths, dir)) plan.output_dir = dir;
	}
	return plan;
}

std::string duplicate_name(const AssetScan &scan, const AssetEntry &file, bool with_record, const BaseNames *base) {
	const auto [stem, extension] = split_name(file.logical_name);
	// The stem's own number, counted on with its width (onjo_m01 makes onjo_m02); none: 1, so the first copy is 2.
	const size_t digits_at = stem.find_last_not_of("0123456789") + 1;
	const std::string prefix = stem.substr(0, digits_at);
	const std::string digits = stem.substr(digits_at);
	const unsigned long long first = digits.empty() ? 1 : strutil::parse_ullong(digits).value_or(1);
	size_t stem_room = std::string::npos;
	if (archive_name_limit_binds(file.kind) && kCopyNameChars > extension.size()) stem_room = kCopyNameChars - extension.size();
	if (file.kind == AssetKind::Model) stem_room = std::min(stem_room, kModelStemChars);
	for (unsigned long long n = first + 1; n < first + 1000; ++n) {
		std::string number = std::to_string(n);
		if (number.size() < digits.size()) number.insert(0, digits.size() - number.size(), '0');
		if (stem_room != std::string::npos && number.size() >= stem_room) break;
		std::string head = prefix;
		if (stem_room != std::string::npos && head.size() + number.size() > stem_room) head.resize(stem_room - number.size());
		if (head.empty() && prefix.size() > 0) break;
		const std::string name = head + number + extension;
		if (copy_name_free(nullptr, scan, file, name, with_record, base)) return name;
	}
	return std::string();
}

DuplicatePlan plan_duplicate(const ProjectPaths &paths, const AssetScan &scan, const std::string &file,
                             const std::string &new_name, bool alone, const BaseNames *base) {
	DuplicatePlan plan;
	const AssetEntry *asset = scan.named(file);
	if (!asset) {
		plan.refusals.push_back(refusal(CoreFinding::FileUnknown, "The project has no file named '" + file + "'.", file));
		return plan;
	}
	plan.path = asset->relative_path;
	plan.name = asset->logical_name;
	if (!asset->imported_from.empty()) {
		plan.refusals.push_back(refusal(CoreFinding::FileImported,
		                                asset->logical_name + " is made by the import of " + asset->imported_from +
		                                        ": duplicate the source, whose copy's import makes its own.",
		                                asset->relative_path));
		return plan;
	}
	const bool source = asset->kind == AssetKind::ImportSource;
	const std::string record = asset->relative_path + kImportSidecarSuffix;
	plan.import = source && !alone && on_disk(paths, record);
	const std::string folder = folder_of_path(asset->relative_path);
	if (new_name.empty()) {
		plan.new_name = duplicate_name(scan, *asset, plan.import, base);
		// The scan's names are free; one on disk the scan has not read yet is tried past.
		for (int tries = 0; !plan.new_name.empty() && on_disk(paths, join_path(folder, plan.new_name)) && tries < 50; ++tries) {
			AssetScan with = scan;
			AssetEntry held;
			held.relative_path = join_path(folder, plan.new_name);
			held.logical_name = plan.new_name;
			with.entries.push_back(held);
			with.index();
			plan.new_name = duplicate_name(with, *asset, plan.import, base);
		}
		if (plan.new_name.empty()) {
			plan.refusals.push_back(refusal(CoreFinding::FileName, "No free name for a copy of " + asset->logical_name + ".",
			                                asset->relative_path));
			return plan;
		}
	} else {
		plan.new_name = new_name;
		FileNameProblem problem = FileNameProblem::None;
		std::string message;
		// A source copied without its record is the file its name alone makes it.
		const AssetKind kind = source && !plan.import ? classify_asset(asset->logical_name, nullptr) : asset->kind;
		if (!check_project_file_name(paths.root, folder, new_name, kind, problem, message)) {
			plan.refusals.push_back(refusal(CoreFinding::FileName, message, asset->relative_path));
			return plan;
		}
		if (!strutil::iequals(split_name(new_name).second, split_name(asset->logical_name).second)) {
			plan.refusals.push_back(refusal(CoreFinding::FileName, "Keep the extension: a file's kind comes from it.",
			                                asset->relative_path));
			return plan;
		}
		std::string why;
		if (!copy_name_free(&paths, scan, *asset, new_name, plan.import, base, &why)) {
			plan.refusals.push_back(refusal(CoreFinding::FileExists, why, asset->relative_path));
			return plan;
		}
	}
	plan.new_path = join_path(folder, plan.new_name);
	plan.copies.emplace_back(asset->relative_path, plan.new_path);
	if (asset->kind == AssetKind::Mission)
		for (const MissionFileSetMember &member : mission_file_set_members(scan, asset->logical_name, plan.new_name)) {
			const AssetEntry *entry = scan.at_path(member.path);
			if (!entry || entry == asset) continue;
			if (!entry->imported_from.empty()) {
				plan.refusals.push_back(refusal(CoreFinding::FileImported,
				                                entry->logical_name + " goes with the mission, and is made by the import of " +
				                                        entry->imported_from + ": duplicate that source first.",
				                                entry->relative_path));
				continue;
			}
			plan.copies.emplace_back(entry->relative_path, join_path(folder_of_path(entry->relative_path), member.new_name));
		}
	if (plan.import) {
		plan.copies.emplace_back(record, plan.new_path + kImportSidecarSuffix);
		for (const AssetEntry &output : scan.entries) {
			if (output.imported_from != asset->relative_path) continue;
			const std::string renamed = renamed_import_output(output.logical_name, asset->logical_name, plan.new_name);
			// An output named otherwise than after its source (its record's own name for it, S18) keeps that name:
			// the copy's import would make a second file of it.
			if (strutil::iequals(renamed, output.logical_name)) {
				plan.refusals.push_back(refusal(CoreFinding::FileName,
				                                asset->logical_name + "'s import names its output " + output.logical_name +
				                                        " whatever the source is called: a copy with its record would make a "
				                                        "second " + output.logical_name +
				                                        ". Duplicate the source alone, then import the copy under a name of "
				                                        "its own.",
				                                asset->relative_path));
				continue;
			}
			plan.outputs.push_back(renamed);
		}
	}
	for (size_t i = 1; i < plan.copies.size(); ++i)
		if (on_disk(paths, plan.copies[i].second))
			plan.refusals.push_back(refusal(CoreFinding::FileExists, "Something sits at " + plan.copies[i].second + " already.",
			                                asset->relative_path));
	return plan;
}

std::string path_in_renamed(const std::string &path, const std::string &from, const std::string &to) {
	if (strutil::iequals(path, from)) return to;
	if (!in_folder(path, from)) return path;
	return to + path.substr(from.size());
}

bool project_folder_of(const ProjectPaths &paths, const ProjectDocument &project, const std::string &text, std::string &out,
                       std::string &why) {
	if (!normalize_project_folder(text, out, why)) return false;
	if (!out.empty() && in_export_folder(system_path(join_path(paths.root, out)), system_path(paths.export_dir(project)))) {
		why = out + " is where the project's export lands: a file there is no file of the project's.";
		return false;
	}
	return true;
}

FolderPlan plan_folder_rename(const ProjectPaths &paths, const ProjectDocument &project, const AssetScan &scan,
                              const std::string &folder, const std::string &new_name,
                              const std::vector<ImportedSource> *imports) {
	FolderPlan plan;
	std::string why;
	if (!project_folder_of(paths, project, folder, plan.from, why)) {
		plan.refusals.push_back(refusal(CoreFinding::FileFolder, why, folder));
		return plan;
	}
	if (plan.from.empty()) {
		plan.refusals.push_back(refusal(CoreFinding::FileFolder, "The top level of the project is no folder to rename.", folder));
		return plan;
	}
	std::error_code ec;
	if (!fs::is_directory(system_path(join_path(paths.root, plan.from)), ec)) {
		plan.refusals.push_back(refusal(CoreFinding::FileUnknown, "The project has no folder " + plan.from + ".", plan.from));
		return plan;
	}
	if (new_name.empty() || new_name == "." || new_name == ".." || new_name.find_first_of("/\\:") != std::string::npos) {
		plan.refusals.push_back(refusal(CoreFinding::FileFolder,
		                                "'" + new_name + "' is not a folder's name: give a name with no folders.", plan.from));
		return plan;
	}
	const std::string parent = folder_of_path(plan.from);
	if (!project_folder_of(paths, project, join_path(parent, new_name), plan.to, why)) {
		plan.refusals.push_back(refusal(CoreFinding::FileFolder, why, plan.from));
		return plan;
	}
	if (strutil::iequals(plan.to, plan.from)) {
		plan.refusals.push_back(refusal(CoreFinding::FileFolder, plan.from + " is named " + new_name + " already.", plan.from));
		return plan;
	}
	if (on_disk(paths, plan.to)) {
		plan.refusals.push_back(refusal(CoreFinding::FileExists, "Something sits at " + plan.to + " already.", plan.from));
		return plan;
	}
	plan.folders.push_back(plan.from);
	for (const auto &[held, stamp] : scan.folders())
		if (in_folder(held, plan.from)) plan.folders.push_back(held);
	std::sort(plan.folders.begin() + 1, plan.folders.end());
	// An import outside the folder that reads a file in it would no longer find it.
	if (imports)
		for (const ImportedSource &source : *imports) {
			if (in_folder(source.source, plan.from)) continue;
			for (const std::string &input : source.inputs)
				if (in_folder(input, plan.from))
					plan.refusals.push_back(refusal(CoreFinding::FileImported,
					                                input + " is read by the import of " + source.source +
					                                        " from where it is: renamed, the folder would hide it from that import.",
					                                input));
		}
	for (const AssetEntry &entry : scan.entries) {
		if (!entry.imported_from.empty() || !in_folder(entry.relative_path, plan.from)) continue;
		RenamePlan move = plan_move(paths, project, scan, entry.relative_path,
		                            folder_of_path(path_in_renamed(entry.relative_path, plan.from, plan.to)));
		// A source whose import reads files beside it moves when every one of them is in the folder too: they are
		// found from its folder, so they move with it and stay found.
		ImportSidecar record;
		if (entry.kind == AssetKind::ImportSource && record_of(paths, entry.relative_path, record) && !record.inputs.empty()) {
			const std::string source_folder = folder_of_path(entry.relative_path);
			const bool inside = std::all_of(record.inputs.begin(), record.inputs.end(), [&](const std::string &input) {
				std::string at, unused;
				return normalize_project_folder(join_path(source_folder, input), at, unused) && in_folder(at, plan.from);
			});
			if (inside)
				move.refusals.erase(std::remove_if(move.refusals.begin(), move.refusals.end(),
				                                   [](const Diagnostic &d) { return d.code() == "rename.imported"; }),
				                    move.refusals.end());
		}
		plan.refusals.insert(plan.refusals.end(), move.refusals.begin(), move.refusals.end());
		plan.moves.push_back(std::move(move));
	}
	return plan;
}

} // namespace opennova::editor
