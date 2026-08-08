// Pins the global sound-bank chain's slot order (audio/bank_chain.h): the
// witnessed six-slot table with the expansion pair ahead of the statics, and
// the four static names alone when no expansion is active.

#include <audio/bank_chain.h>

#include <cstdio>
#include <string>
#include <vector>

int main() {
	using opennova::audio::global_bank_chain;

	const std::vector<std::string> base = global_bank_chain("");
	const std::vector<std::string> expect_base = {
			"gamelocl.lwf", "game.lwf", "game3.lwf", "game2.lwf"};
	if (base != expect_base) {
		std::fprintf(stderr, "FAIL: base chain order\n");
		return 1;
	}

	const std::vector<std::string> exp = global_bank_chain("jox01");
	const std::vector<std::string> expect_exp = {
			"jox01L.lwf", "jox01.lwf",
			"gamelocl.lwf", "game.lwf", "game3.lwf", "game2.lwf"};
	if (exp != expect_exp) {
		std::fprintf(stderr, "FAIL: expansion chain order\n");
		return 1;
	}

	std::printf("bank_chain_test: OK\n");
	return 0;
}
