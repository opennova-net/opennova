#include <runtime/world/title_rules.h>

#include <base/io/strutil.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace opennova::world {

namespace {

std::vector<TitleRulesRow> &registry() {
	static std::vector<TitleRulesRow> rows;
	return rows;
}

} // namespace

void install_title_rules(const TitleRulesRow *rows, size_t count) {
	registry().assign(rows, rows + count);
}

const TitleRules &title_rules(std::string_view title_code) {
	const std::vector<TitleRulesRow> &rows = registry();
	if (rows.empty()) {
		std::fputs("no title rules installed: the program must call games::install_rules()\n", stderr);
		std::abort();
	}
	for (const TitleRulesRow &row : rows)
		if (strutil::iequals(row.title_code, title_code))
			return *row.rules;
	// No default title's rules stand in for a code no title has, an empty one included: the caller
	// names its session's title (ADR 0076, 2026-10-30).
	std::fputs("world::title_rules: no title rules are installed under the code \"", stderr);
	std::fwrite(title_code.data(), 1, title_code.size(), stderr);
	std::fputs("\": games::install_rules() registers every title's\n", stderr);
	std::abort();
}

void title_rules_unset(const char *holder) {
	std::fputs(holder, stderr);
	std::fputs(": no title rules: the session sets them (world::title_rules) before they are read\n",
			stderr);
	std::abort();
}

} // namespace opennova::world
