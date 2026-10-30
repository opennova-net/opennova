#include "jodemo.h"

namespace opennova::games {

namespace {

Protocol make() {
	Protocol p;
	p.code = "jodemo";
	p.name = "JO demo";
	// docs/net/novaworld-net-re.md §6.7.
	p.gate_tag = "jopd:cus4";
	// Its engine build, which no protocol carries (ONE's build fills the engine-build slots):
	// "V%i.%i.%i.%i" over 1, 0, 0, 9, ebx = 1 and ebp = 0 [orig: jodemo.exe @ 0x48ad10 — ebx
	// @ 0x48ad52, ebp @ 0x48b33e, the sprintf @ 0x48b440..0x48b459, the dword 0x01000009
	// @ 0x48b44f].
	return p;
}

} // namespace

const Protocol &jodemo_protocol() {
	static const Protocol protocol = make();
	return protocol;
}

} // namespace opennova::games
