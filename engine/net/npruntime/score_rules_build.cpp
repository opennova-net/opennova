#include <net/npruntime/score_rules_build.h>

namespace opennova::np {

world::ScoreRules build_score_rules(const score::File &config, uint32_t game_type) {
	world::ScoreRules rules;
	const int row = score::row_for_game_type(game_type);
	if (row < 0) return rules; // the loader's own `<= 11` guard [orig: @0x52D300]
	const score::GameTypeBlock *block = score::block_at(config, row);
	if (block == nullptr) return rules;
	// Name lookups, not slot arithmetic. Names are the shipped table's
	// [orig: off_830348 @ 0x830348].
	rules.enemy_kill = score::var_value(*block, "ENEMYKILL", 0);       // slot 3
	rules.friendly_kill = score::var_value(*block, "FRIENDLYKILL", 0); // slot 2
	rules.suicide = score::var_value(*block, "SUICIDE", 0);            // slot 4
	rules.death = score::var_value(*block, "DEATH", 0);                // slot 5
	rules.valid = true;
	return rules;
}

} // namespace opennova::np
