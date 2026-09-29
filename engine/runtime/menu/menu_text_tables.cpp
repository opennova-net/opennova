// The menu's string tables [orig: CUIStringTable_LookupString @ 0x6527c0].

#include <runtime/menu/menu_text_tables.h>

#include <base/io/strutil.h>

#include <utility>

namespace opennova::menu {

void MenuTextTables::clear() {
	override_ = nullptr;
	tables_.clear();
}

void MenuTextTables::set_table(const std::string &name, std::shared_ptr<const rtxt::File> file) {
	tables_[strutil::to_lower(name)] = std::move(file);
}

bool MenuTextTables::has_table(const std::string &name) const {
	return tables_.count(strutil::to_lower(name)) != 0;
}

bool MenuTextTables::loaded(const std::string &name) const {
	const auto found = tables_.find(strutil::to_lower(name));
	return found != tables_.end() && found->second != nullptr;
}

const std::string *MenuTextTables::lookup(const std::string *name, const std::string &key) const {
	if (name == nullptr) return nullptr;
	const auto found = tables_.find(strutil::to_lower(*name));
	if (found == tables_.end() || !found->second) return nullptr;
	if (override_ != nullptr) {
		if (const rtxt::Entry *entry = override_->find_in_section(kMenuTextSection, key)) return &entry->text;
	}
	const rtxt::Entry *entry = found->second->find_in_section(kMenuTextSection, key);
	return entry != nullptr ? &entry->text : nullptr;
}

} // namespace opennova::menu
