#include <runtime/audio/bank_chain.h>

namespace opennova::audio {

std::vector<std::string> global_bank_chain(const std::string &expansion_name) {
	std::vector<std::string> chain;
	chain.reserve(6);
	if (!expansion_name.empty()) {
		chain.push_back(expansion_name + "L.lwf"); // [orig: Expansion_LoadAssets @ 0x4a4989]
		chain.push_back(expansion_name + ".lwf");  // [orig: Expansion_LoadAssets @ 0x4a495e]
	}
	// The static slots, byte-for-byte [orig: @ 0x82A7B8..0x82AAC4].
	chain.push_back("gamelocl.lwf");
	chain.push_back("game.lwf");
	chain.push_back("game3.lwf");
	chain.push_back("game2.lwf");
	return chain;
}

} // namespace opennova::audio
