// D-NET-352 (Q7-6): opennova-serve keeps no player.sav, so the profile its own row runs under is
// the boot's seed: every record fresh, the first named after the process's account through
// GetUserNameA's 16-byte buffer (a name it cannot hold with its NUL leaves the field empty), the
// current one game.cfg's player_index (an index past the records reads the first). The mission
// start copies that name onto the row's body.
// [orig: PlayerProfile_LoadAllFromDisk @0x54F4D0 - InitDefaults @0x54F51B, GetUserNameA
//  @0x54F535, no player.sav @0x54F586, the current record @0x54F65D..0x54F682]

#include "profile_name.h"

#include <cstdio>
#include <string>

namespace {

int failures = 0;
#define CHECK(c)                                                                               \
	do {                                                                                       \
		if (!(c)) {                                                                            \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                           \
			++failures;                                                                        \
		}                                                                                      \
	} while (0)

void test_the_first_record_takes_the_account() {
	using opennova::serve::serve_profile_name;
	CHECK(serve_profile_name(0, "cdouglass") == "cdouglass");
	// Fifteen characters fit the buffer with the NUL; sixteen fail and leave it empty.
	CHECK(serve_profile_name(0, std::string(15, 'u')) == std::string(15, 'u'));
	CHECK(serve_profile_name(0, std::string(16, 'u')).empty());
	CHECK(serve_profile_name(0, std::string()).empty());
	// Any other record is a nameless fresh one; an index past the records reads the first.
	CHECK(serve_profile_name(3, "cdouglass").empty());
	CHECK(serve_profile_name(6, "cdouglass") == "cdouglass");
	CHECK(serve_profile_name(-1, "cdouglass") == "cdouglass");
}

// The account read gives a name the load can judge (the host's own login here).
void test_the_account_name_reads() {
	const std::string name = opennova::serve::process_account_name();
	CHECK(name.find('\0') == std::string::npos);
}

} // namespace

int main() {
	test_the_first_record_takes_the_account();
	test_the_account_name_reads();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("PASS opennova_serve_profile_name\n");
	return 0;
}
