#include <editor/session/texture_import_state.h>

#include <set>
#include <tuple>

#include <base/io/strutil.h>
#include <editor/assets/asset_registry.h>
#include <editor/project/project_files.h>
#include <editor/session/texture_use_index.h>
#include <editor/session/view/session_view.h>

namespace opennova::editor {

namespace {

using io::JsonValue;
using io::json_number;
using io::json_string;

JsonValue strings_json(const std::vector<std::string> &list) {
	JsonValue out = JsonValue::make_array();
	for (const std::string &each : list) out.push(json_string(each));
	return out;
}

JsonValue options_json(const ImportOptions &options) {
	JsonValue out = JsonValue::make_object();
	for (const auto &[key, value] : options) out.set(key, json_string(value));
	return out;
}

} // namespace

bool texture_import_state(const SessionView &view, const std::string &path, TextureImportState &out,
                          std::string &error) {
	out = TextureImportState();
	if (!view.project.open || !view.project.scan) {
		error = "no project is open.";
		return false;
	}
	const AssetScan &scan = *view.project.scan;
	const AssetEntry *entry = scan.at_path(path);
	if (!entry) entry = scan.find(path);
	if (!entry) {
		error = "no file " + path + " in the project.";
		return false;
	}
	out.source = entry->imported_from.empty() ? entry->relative_path : entry->imported_from;
	const AssetEntry *source = scan.at_path(out.source);
	if (!source || source->kind != AssetKind::ImportSource) {
		error = entry->relative_path + " is not imported: no import record makes it.";
		return false;
	}
	out.record = out.source + kImportSidecarSuffix;
	Diagnostic read_error;
	if (!load_import_sidecar(join_path(view.project.root, out.record), out.sidecar, read_error)) {
		error = out.record + " does not read" + (read_error.message.empty() ? "." : ": " + read_error.message);
		return false;
	}
	for (const Importer &importer : importers())
		if (out.sidecar.importer == importer.id) out.importer = &importer;
	if (!out.importer) {
		error = out.record + " names the importer '" + out.sidecar.importer + "', which the editor does not have.";
		return false;
	}
	for (const AssetEntry &each : scan.entries)
		if (each.imported_from == out.source) out.outputs.push_back(each.relative_path);
	// The uses of what it makes and of every name of its stem, each once.
	std::vector<TextureUse> uses;
	if (view.documents.texture_uses) {
		std::set<std::tuple<std::string, std::string, std::string, std::string, std::string>> seen;
		const auto take = [&](const std::vector<TextureUse> &list) {
			for (const TextureUse &use : list)
				if (seen.insert({use.referrer, use.locator, use.field, strutil::to_lower(use.name_written), use.fixed_for}).second)
					uses.push_back(use);
		};
		for (const std::string &output : out.outputs) take(view.documents.texture_uses->uses_of(view, output));
		take(view.documents.texture_uses->uses_named(view, utf8_of(path_of(basename_of(out.source)).stem())));
	}
	out.needs = texture_import_needs(uses, basename_of(out.source));
	return true;
}

TextureUseAsks texture_use_asks(const SessionView &view, const std::string &path) {
	TextureUseAsks out;
	if (!view.documents.texture_uses || !view.project.scan) return out;
	const AssetEntry *entry = view.project.scan->at_path(path);
	if (!entry) entry = view.project.scan->find(basename_of(path));
	std::vector<TextureUse> uses;
	if (entry) {
		uses = view.documents.texture_uses->uses_of(view, entry->relative_path);
	} else {
		// A name the project lacks: the uses writing it.
		for (const TextureUse &use : view.documents.texture_uses->uses_named(view, utf8_of(path_of(basename_of(path)).stem())))
			if (normalized_logical_name(basename_of(use.name_written)) == normalized_logical_name(basename_of(path))) uses.push_back(use);
	}
	std::set<std::string> sizes;
	for (const TextureUse &use : uses) {
		// A use that opens another file of the name asks nothing of this one (a missing name's uses read it).
		if (!use.known() || (entry && !use.reads_file)) continue;
		if ((use.role == TextureRoleId::TerrainFoliageMap || use.role == TextureRoleId::TerrainCharMap) && !out.indices) {
			out.indices = true;
			out.indices_why = use.words;
		}
		const TextureRoleRow &row = texture_role_row(use.role);
		if (row.size == TextureSizeRule::Exact) {
			const std::string size = std::to_string(row.width) + "x" + std::to_string(row.height);
			if (sizes.insert(size).second && out.size.empty()) out.size_why = use.words + " reads it at " + texture_size_words(row);
			out.size = size;
		}
	}
	if (sizes.size() > 1) {
		out.size.clear();
		out.size_why.clear();
	}
	return out;
}

std::string import_option_value(const TextureImportState &state, const ImportOptionRow &row) {
	const auto found = state.sidecar.options.find(row.key);
	return found == state.sidecar.options.end() || found->second.empty() ? row.fallback : found->second;
}

bool import_option_applies_now(const TextureImportState &state, const ImportOptionRow &row) {
	if (row.applies_to.empty() || !state.importer) return true;
	const ImportOptionRow *under = import_option_row(state.importer->options, row.applies_to);
	if (!under) return true;
	if (!import_option_applies_now(state, *under)) return false;
	const std::string value = strutil::to_lower(import_option_value(state, *under));
	for (const std::string &each : row.applies_values)
		if (value == each) return true;
	return false;
}

JsonValue texture_import_state_json(const TextureImportState &state) {
	JsonValue out = JsonValue::make_object();
	out.set("source", json_string(state.source));
	out.set("record", json_string(state.record));
	out.set("importer", json_string(state.sidecar.importer));
	out.set("version", json_number(double(state.sidecar.version)));
	out.set("options", options_json(state.sidecar.options));
	JsonValue effective = JsonValue::make_object();
	JsonValue rows = JsonValue::make_array();
	if (state.importer)
		for (const ImportOptionRow &row : state.importer->options) {
			effective.set(row.key, json_string(import_option_value(state, row)));
			JsonValue entry = JsonValue::make_object();
			entry.set("key", json_string(row.key));
			entry.set("label", json_string(row.label));
			entry.set("words", json_string(row.words));
			JsonValue values = JsonValue::make_array();
			for (const ImportOptionValue &value : row.values) {
				JsonValue item = JsonValue::make_object();
				item.set("token", json_string(value.token));
				item.set("words", json_string(value.words));
				values.push(std::move(item));
			}
			entry.set("values", std::move(values));
			entry.set("forms", strings_json(row.forms));
			entry.set("fallback", json_string(row.fallback));
			JsonValue applies = JsonValue::make_null();
			if (!row.applies_to.empty()) {
				applies = JsonValue::make_object();
				applies.set("option", json_string(row.applies_to));
				applies.set("values", strings_json(row.applies_values));
			}
			entry.set("applies", std::move(applies));
			entry.set("applies_now", JsonValue::make_bool(import_option_applies_now(state, row)));
			rows.push(std::move(entry));
		}
	out.set("effective", std::move(effective));
	out.set("rows", std::move(rows));
	out.set("outputs", strings_json(state.outputs));
	JsonValue needs = JsonValue::make_object();
	needs.set("options", options_json(state.needs.options));
	needs.set("reasons", strings_json(state.needs.reasons));
	needs.set("conflicts", strings_json(state.needs.conflicts));
	needs.set("split_referrers", strings_json(state.needs.split_referrers));
	needs.set("uses", json_number(double(state.needs.uses)));
	out.set("needs", std::move(needs));
	return out;
}

} // namespace opennova::editor
