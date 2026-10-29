#include <base/title/title.h>

#include <base/io/strutil.h>
#include <base/vfs/vfs_decode.h>
#include <formats/pff/pff.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace opennova {

// A new archive of any title is PFF3, which every JO-family title's archive open takes
// [orig: PFF_Open @ 0x7682e0].
static_assert(kNewArchivePffFormat == pff::PFF_FORMAT_PFF3, "a new archive is PFF3 (ADR 0008)");

namespace {

struct Registry {
	std::vector<const Title *> titles;
	const Title *fallback = nullptr;
};

Registry &registry() {
	static Registry r;
	return r;
}

// Every lookup's precondition: the program installed its titles.
const Registry &installed() {
	const Registry &r = registry();
	if (r.titles.empty() || r.fallback == nullptr) {
		std::fputs("no titles installed: the program must call games::install()\n", stderr);
		std::abort();
	}
	return r;
}

} // namespace

void install_titles(const Title *const *titles, size_t count, const Title *default_title) {
	Registry &r = registry();
	r.titles.assign(titles, titles + count);
	r.fallback = default_title;
}

const Title *title_by_code(std::string_view code) {
	for (const Title *title : installed().titles)
		if (strutil::iequals(title->code, code))
			return title;
	return nullptr;
}

const Title &default_title() {
	return *installed().fallback;
}

size_t title_count() {
	return installed().titles.size();
}

const Title *title_at(size_t index) {
	const Registry &r = installed();
	return index < r.titles.size() ? r.titles[index] : nullptr;
}

const Title *title_by_command_file(std::string_view command_file) {
	for (const Title *title : installed().titles)
		if (title->command_file != nullptr && strutil::iequals(title->command_file, command_file))
			return title;
	return nullptr;
}

int title_scr_policy_for_code(const char *code) {
	const Title *title = code != nullptr ? title_by_code(code) : nullptr;
	return title != nullptr ? title->scr_policy : VFS_SCR_VERSION_DETECT;
}

gameprofile::GameId title_game_id(const Title &title) {
	return std::strcmp(title.code, "dfx2") == 0 ? gameprofile::GAME_DFX2 : gameprofile::GAME_JO;
}

} // namespace opennova
