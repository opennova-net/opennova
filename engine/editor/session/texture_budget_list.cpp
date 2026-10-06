#include <editor/session/texture_budget_list.h>

#include <algorithm>
#include <map>
#include <utility>

#include <base/io/strutil.h>
#include <editor/assets/asset_registry.h>
#include <editor/graph/texture_uses.h>
#include <editor/session/texture_use_index.h>
#include <editor/session/view/session_view.h>

namespace opennova::editor {

TextureBudgetList texture_budget_list(const SessionView &view) {
	TextureBudgetList out;
	if (!view.project.scan || !view.documents.texture_uses) return out;
	// Each use costed once, through the file its loader opens; keyed as the game keeps the texture: the name
	// written, any case, and whether the normal-map loader made it.
	std::map<std::pair<std::string, bool>, size_t> made;
	for (const AssetEntry &entry : view.project.scan->entries) {
		if (entry.kind != AssetKind::Texture) continue;
		for (const TextureUse &use : view.documents.texture_uses->uses_of(view, entry.relative_path)) {
			if (!use.budget.known || !use.reads_file || use.served != entry.relative_path) continue;
			const auto key = std::make_pair(strutil::to_lower(use.name_written), use.budget.loader == TextureBudgetLoader::Normal);
			const auto found = made.find(key);
			if (found != made.end()) {
				++out.rows[found->second].uses;
				continue;
			}
			made.emplace(key, out.rows.size());
			out.rows.push_back({use.name_written, use.served, use.budget, 1});
		}
	}
	std::stable_sort(out.rows.begin(), out.rows.end(), [](const TextureBudgetRow &a, const TextureBudgetRow &b) {
		return a.budget.full().bytes > b.budget.full().bytes;
	});
	for (const TextureBudgetRow &row : out.rows) {
		for (int level = 0; level < renderer::kObjectTexDetailLevels; ++level) out.at_detail[level] += row.budget.detail[level].bytes;
		out.as_dds += row.budget.offers_dds ? row.budget.as_dds.bytes : row.budget.full().bytes;
		out.past_warning += row.budget.full().bytes > kTextureMemoryWarnBytes ? 1 : 0;
	}
	return out;
}

std::string texture_file_budget_words(const SessionView &view, const std::string &file) {
	if (!view.documents.texture_uses) return "";
	for (const TextureUse &use : view.documents.texture_uses->uses_of(view, file))
		if (use.reads_file && use.budget.known) return texture_budget_words(use.budget);
	return "";
}

io::JsonValue texture_budget_row_json(const TextureBudgetRow &row) {
	io::JsonValue out = io::JsonValue::make_object();
	out.set("name", io::json_string(row.name));
	out.set("file", io::json_string(row.file));
	out.set("uses", io::json_number(double(row.uses)));
	out.set("budget", texture_budget_json(row.budget));
	return out;
}

io::JsonValue texture_budget_totals_json(const TextureBudgetList &list) {
	io::JsonValue out = io::JsonValue::make_object();
	io::JsonValue detail = io::JsonValue::make_array();
	for (int level = 0; level < renderer::kObjectTexDetailLevels; ++level) detail.push(io::json_number(double(list.at_detail[level])));
	out.set("detail", std::move(detail));
	out.set("as_dds", io::json_number(double(list.as_dds)));
	out.set("textures", io::json_number(double(list.rows.size())));
	out.set("past_warning", io::json_number(double(list.past_warning)));
	return out;
}

} // namespace opennova::editor
