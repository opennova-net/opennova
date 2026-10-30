#include <runtime/world/title_rules.h>

#include <base/io/strutil.h>
#include <base/title/title.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace opennova::world {

namespace {

std::vector<TitleRulesRow> &registry() {
	static std::vector<TitleRulesRow> rows;
	return rows;
}

const TitleRules *&unset_slot() {
	static const TitleRules *rules = nullptr;
	return rules;
}

} // namespace

void install_unset_title_rules(const TitleRules *rules) {
	unset_slot() = rules;
}

const TitleRules *unset_title_rules() {
	return unset_slot();
}

void install_title_rules(const TitleRulesRow *rows, size_t count) {
	registry().assign(rows, rows + count);
}

const TitleRules &title_rules(std::string_view title_code) {
	const std::vector<TitleRulesRow> &rows = registry();
	if (rows.empty()) {
		std::fputs("no title rules installed: the program must call games::install_rules()\n", stderr);
		std::abort();
	}
	const std::string_view fallback = default_title().code;
	for (const std::string_view code : { title_code, fallback })
		for (const TitleRulesRow &row : rows)
			if (strutil::iequals(row.title_code, code))
				return *row.rules;
	std::fputs("the default title has no rules: games::install_rules() registers every title's\n", stderr);
	std::abort();
}

void title_rules_unset(const char *holder) {
	std::fputs(holder, stderr);
	std::fputs(": no title rules: the session sets them (world::title_rules) before they are read\n",
			stderr);
	std::abort();
}

} // namespace opennova::world
