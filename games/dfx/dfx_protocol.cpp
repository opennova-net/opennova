#include "dfx.h"

namespace opennova::games {

namespace {

Protocol make() {
	Protocol p;
	p.code = "dfx";
	p.name = "DFX";
	// Its engine build, which no protocol carries (ONE's build fills the engine-build slots):
	// "V%i.%i.%i.%i" over 1, 7, 1, 9, ebx = 1 [orig: DFX.EXE @ 0x4a6180 — ebx @ 0x4a61eb, the
	// sprintf @ 0x4a6b93..0x4a6bad, the dword 0x01070109 @ 0x4a6ba3].
	return p;
}

} // namespace

const Protocol &dfx_protocol() {
	static const Protocol protocol = make();
	return protocol;
}

} // namespace opennova::games
