#include "bhd.h"

namespace opennova::games {

namespace {

Protocol make() {
	Protocol p;
	p.code = "bhd";
	p.name = "BHD";
	return p;
}

} // namespace

const Protocol &bhd_protocol() {
	static const Protocol protocol = make();
	return protocol;
}

} // namespace opennova::games
