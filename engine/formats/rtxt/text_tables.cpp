#include "text_tables.h"

#include <base/io/strutil.h>

#include <unordered_map>
#include <utility>

namespace opennova::rtxt {

namespace {

// The registry by lower-cased name: one slot per table the game loads at init and per menu or
// mission table [orig: Game_InitSubsystems @0x4A6CD0].
std::unordered_map<std::string, std::shared_ptr<const File>> &tables() {
	static std::unordered_map<std::string, std::shared_ptr<const File>> registry;
	return registry;
}

std::string lowered(std::string_view name) { return strutil::to_lower(name); }

} // namespace

void register_table(std::string_view name, std::shared_ptr<const File> table) {
	if (table == nullptr) {
		tables().erase(lowered(name));
		return;
	}
	tables()[lowered(name)] = std::move(table);
}

std::shared_ptr<const File> registered_table(std::string_view name) {
	const auto it = tables().find(lowered(name));
	return it != tables().end() ? it->second : nullptr;
}

void clear_tables() { tables().clear(); }

bool table_text(std::string_view name, const std::string &section, const std::string &key,
		std::string &out) {
	const std::shared_ptr<const File> table = registered_table(name);
	if (table == nullptr) return false;
	const Entry *entry = find_with_text_override(*table, section, key);
	if (entry == nullptr) return false;
	out = entry->text;
	return true;
}

std::string table_text_or(std::string_view name, const std::string &section,
		const std::string &key, const std::string &fallback) {
	std::string out;
	return table_text(name, section, key, out) ? out : fallback;
}

std::string table_lookup(std::string_view name, const std::string &section,
		const std::string &key) {
	const std::shared_ptr<const File> table = registered_table(name);
	return lookup_with_override(text_override().get(), table.get(), section, key);
}

std::string table_lookup_display(std::string_view name, const std::string &section,
		const std::string &key) {
	int hotkey = -1;
	return strip_hotkey(table_lookup(name, section, key), hotkey);
}

void install_text_override_bytes(const uint8_t *data, size_t size) {
	auto table = std::make_shared<File>();
	std::string error;
	if (size > 0 && parse(data, size, *table, error)) {
		table->build_lookup();
		set_text_override(std::move(table));
	} else {
		set_text_override(nullptr);
	}
}

// [orig: TextResource_GetStringWithFallback(resource, "Menu", key) @0x562EE0]
bool find_menu_text(const std::string &key, std::string &out) {
	return table_text(kTableMenuTxt, kSectionMenu, key, out) ||
			table_text(kTableGameUi, kSectionMenu, key, out);
}

std::string menu_text(const std::string &key, const std::string &fallback) {
	std::string out;
	return find_menu_text(key, out) ? out : fallback;
}

} // namespace opennova::rtxt
