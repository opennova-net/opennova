// The games games::install() registers (games/): every installed title resolves and speaks a
// protocol, JO's and DFX2's complete for the session and the web flow, OP: Acid Gambit speaking
// JO's very object, the unwitnessed titles refused by name, and a PN that two titles' one
// protocol carries resolving to that one object. And the rules each title runs
// (runtime/world/title_rules.h): JO's the defaults, DFX2's its witnessed knobs, OP: Acid Gambit's
// and the unwitnessed titles' JO's very object, and each game's bump-rule row (its ProtocolInMatch
// and its joiner-side knobs), a change to which bumps the game's protocol version
// (games/CLAUDE.md).

#include <base/title/title.h>
#include <games/acidgambit/acidgambit.h>
#include <games/dfx2/dfx2.h>
#include <games/jo/jo.h>
#include <games/novalogic_2009/generation.h>
#include <net/novacrypto/cdkey.h>
#include <net/protocol/protocol.h>
#include <runtime/world/title_rules.h>

#include <cstdio>
#include <string>

using namespace opennova;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                                \
	do {                                                                                           \
		if (!(cond)) {                                                                             \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                            \
			++g_failures;                                                                          \
		}                                                                                          \
	} while (0)

// Every installed title resolves by its code and speaks an installed protocol.
void every_title_resolves() {
	CHECK(title_count() == 6);
	for (size_t i = 0; i < title_count(); ++i) {
		const Title *title = title_at(i);
		CHECK(title != nullptr && title_by_code(title->code) == title);
		const Protocol *protocol = title != nullptr ? title_protocol(title->code) : nullptr;
		CHECK(protocol != nullptr && protocol->code != nullptr && protocol->name != nullptr);
		if (protocol != nullptr)
			CHECK(protocol_by_code(protocol->code) == protocol);
	}
	CHECK(title_protocol("nosuchgame") == nullptr && protocol_by_code("nosuchgame") == nullptr);
	// The title a launch, a project or a session that names none runs as: OpenNova's own game.
	CHECK(&default_title() == title_by_code("acidgambit"));
}

// JO's and DFX2's protocols are complete for the session and the web flow, each the games'
// accessor's object over the 2009 generation, each named for its protocol line.
void retail_protocols_complete() {
	const Protocol *jo = title_protocol("jo");
	const Protocol *dfx2 = title_protocol("dfx2");
	CHECK(jo == &games::jo_protocol() && dfx2 == &games::dfx2_protocol());
	CHECK(&games::jo_title() == title_by_code("jo") &&
			&games::dfx2_title() == title_by_code("dfx2"));
	for (const Protocol *protocol : { jo, dfx2 }) {
		CHECK(protocol != nullptr);
		if (protocol == nullptr)
			continue;
		CHECK(protocol_complete(*protocol, ProtocolPart::Session));
		CHECK(protocol_complete(*protocol, ProtocolPart::WebFlow));
		CHECK(protocol->generation == &games::novalogic_2009());
		CHECK(std::string(protocol_version(*protocol)) == "16");
		std::string error;
		CHECK(resolve_title_protocol(protocol->code, error) == protocol && error.empty());
	}
	CHECK(std::string(jo->name) == "JO" && std::string(dfx2->name) == "DFX2");
	CHECK(jo != dfx2);
	// DFX2's protocol carries the CD key its LAN host checks (DFX2-13); JO's has none.
	CHECK(dfx2->cd_key != nullptr && dfx2->cd_key->wire != nullptr &&
			dfx2->cd_key->registry != nullptr && dfx2->cd_key->exp1 != nullptr);
	CHECK(jo->cd_key == nullptr);
}

// OP: Acid Gambit speaks JO's protocol: the same object, no copied literals, so a
// JOINTOPERATIONS peer resolves to the one protocol whichever title registered it.
void acidgambit_speaks_jo() {
	const Protocol *jo = title_protocol("jo");
	CHECK(title_protocol("acidgambit") == jo);
	CHECK(&games::acidgambit_protocol() == &games::jo_protocol());
	CHECK(protocol_by_pn("JOINTOPERATIONS") == jo && protocol_by_pn("jointoperations") == jo);
	std::string error;
	CHECK(resolve_title_protocol("acidgambit", error) == jo);
	const Title &ag = games::acidgambit_title();
	CHECK(title_by_code("ACIDGAMBIT") == &ag);
	CHECK(std::string(ag.display_name) == "OP: Acid Gambit" &&
			std::string(ag.title_text) == "OP: Acid Gambit");
	CHECK(std::string(ag.command_file) == "ACIDGAMBIT.CMD" && ag.executable == nullptr);
	CHECK(ag.scr_policy == games::jo_title().scr_policy);
	CHECK(protocol_by_pn("DELTAFORCEXTREME2") == title_protocol("dfx2"));
}

// The titles no binary has been read for: refused by name, as before the registry.
void unwitnessed_titles_refused() {
	std::string error;
	CHECK(resolve_title_protocol("bhd", error) == nullptr);
	CHECK(error.find("the bhd title's session identity is not witnessed yet: NVS") == 0);
	CHECK(resolve_title_protocol("dfx", error) == nullptr);
	CHECK(error.find("the dfx title's session identity is not witnessed yet: NVS") == 0);
	CHECK(resolve_title_protocol("jodemo", error) == nullptr);
	CHECK(error.find("PG") != std::string::npos && error.find("gate tag") == std::string::npos);
	const Protocol *demo = title_protocol("jodemo");
	CHECK(demo != nullptr && std::string(demo->gate_tag) == "jopd:cus4");
	CHECK(protocol_refusal(*demo, ProtocolPart::WebFlow) ==
			"the jodemo title's web flow identity is not witnessed yet: sign-in submit");
	CHECK(resolve_title_protocol("nosuchgame", error) == nullptr);
	CHECK(error == "unknown game 'nosuchgame'");
}

// Every installed title runs installed rules: JO's for jo and for the titles that run JO's (the
// same object), DFX2's its own; an empty or unknown code runs the default title's.
void every_title_runs_rules() {
	const world::TitleRules &jo = world::title_rules("jo");
	CHECK(&jo == &games::jo_rules());
	CHECK(&world::title_rules("JO") == &jo);
	CHECK(&world::title_rules("dfx2") == &games::dfx2_rules() && &games::dfx2_rules() != &jo);
	for (const char *code : { "acidgambit", "jodemo", "dfx", "bhd" })
		CHECK(&world::title_rules(code) == &jo);
	CHECK(&games::acidgambit_rules() == &jo);
	CHECK(&world::title_rules("") == &jo && &world::title_rules("nosuchgame") == &jo);
}

// JO's rules are the defaults (Jointops.exe's, the baseline the ports were witnessed on); DFX2's
// each knob's dfx2.exe value (docs/titles/dfx2-vs-jo-re.md §8, the rows each names).
void retail_rules_values() {
	const world::TitleRules &jo = world::title_rules("jo");
	CHECK(jo.waypoint_completion == world::WaypointCompletion::LinkedSpawnPoints);
	CHECK(!jo.coop_checkpoint_spawn && jo.waypoint_goals_met_advance &&
			!jo.waypoint_advance_needs_done && !jo.route_done_records);
	CHECK(jo.score_values == &world::builtin_score_values && jo.score_exp_fanfare == 0 &&
			!jo.round_win_score_multiplier && !jo.vehicle_attach_kill_scored);
	CHECK(jo.hit_zone_damage_scale == &world::hit_zone_damage_multiplier &&
			!jo.impact_damage_clamp_first && jo.occupant_damage_scale);
	CHECK(jo.round_end_linger_ticks == 2790u && jo.deploy_target_protection_ticks == 620);
	CHECK(jo.medic_heal_and_revive && jo.downed_state_resend && jo.revive_window);
	CHECK(jo.emplaced_turret_update == world::EmplacedTurretUpdate::Plain &&
			jo.emplaced_solo_planar_slew == 0 && !jo.narrow_still_player_radius);
	CHECK(jo.ai_vehicle_near_reach == 0xC0000 && jo.ai_vehicle_write_off_count == 3410 &&
			jo.spawn_kit == nullptr);

	const world::TitleRules &dfx2 = world::title_rules("dfx2");
	CHECK(dfx2.waypoint_completion == world::WaypointCompletion::CoopCheckpoint); // DFX2-9
	CHECK(dfx2.coop_checkpoint_spawn && !dfx2.waypoint_goals_met_advance &&
			dfx2.waypoint_advance_needs_done && dfx2.route_done_records);
	CHECK(dfx2.score_values != &world::builtin_score_values && // DFX2-15
			dfx2.score_values(0x10000u)[3] == 2 && dfx2.score_values(0x10000u)[37] == 6 &&
			dfx2.score_exp_fanfare == (4u | (10u << 8)) && dfx2.round_win_score_multiplier &&
			dfx2.vehicle_attach_kill_scored);
	CHECK(dfx2.hit_zone_damage_scale(0) == 1.0 && dfx2.hit_zone_damage_scale(13) == 3.0 && // DFX2-16
			dfx2.impact_damage_clamp_first && !dfx2.occupant_damage_scale);
	CHECK(dfx2.round_end_linger_ticks == 1550u && dfx2.deploy_target_protection_ticks == 310); // 17
	CHECK(!dfx2.medic_heal_and_revive && !dfx2.downed_state_resend && !dfx2.revive_window); // 19, 20
	CHECK(dfx2.emplaced_turret_update == world::EmplacedTurretUpdate::CounterRotate && // DFX2-25
			dfx2.emplaced_solo_planar_slew == 0x3AB948 && dfx2.narrow_still_player_radius); // 26, 31
	CHECK(dfx2.ai_vehicle_near_reach == 0x40000 && dfx2.ai_vehicle_write_off_count == 1860); // 32
	size_t kit = 0;
	while (dfx2.spawn_kit != nullptr && dfx2.spawn_kit[kit] != nullptr) ++kit; // DFX2-33
	CHECK(kit == 8 && std::string(dfx2.spawn_kit[2]) == "WPN_MK23");
}

// The bump-rule row (plan decision 12): what a game's joiner reads or writes as it does, so a
// change to a game's row lands with a bump of its protocol version in its <code>_protocol.cpp
// (games/CLAUDE.md). It is the game's ProtocolInMatch, every field of which shapes the bytes the
// host sends or the joiner sends (docs/titles/dfx2-vs-jo-re.md §8), and its TitleRules fields
// marked joiner-side (read on a joiner's world or its replica). JO's and DFX2's rows are
// retail-pinned and never change.
std::string bump_rule_row(const char *title_code) {
	const Protocol *protocol = title_protocol(title_code);
	if (protocol == nullptr || !protocol->in_match.has_value())
		return "no in-match protocol";
	const ProtocolInMatch &m = *protocol->in_match;
	const world::TitleRules &r = world::title_rules(title_code);
	char rules_word[16];
	std::snprintf(rules_word, sizeof(rules_word), "0x%X", static_cast<unsigned>(m.rules_word_bits));
	std::string kit;
	for (const char *const *name = r.spawn_kit; name != nullptr && *name != nullptr; ++name)
		kit += std::string(kit.empty() ? "" : ",") + *name;
	return "objective_checkpoint=" + std::to_string(m.objective_checkpoint) +
			" spawn_zone_flag_always=" + std::to_string(m.spawn_zone_flag_always) +
			" armory_only_availability=" + std::to_string(m.armory_only_availability) +
			" join_state_fixed=" + std::to_string(m.join_state_fixed) +
			" loadout_class_fixed=" + std::to_string(m.loadout_class_fixed) +
			" lan_cd_key_check=" + std::to_string(m.lan_cd_key_check) +
			" chat_squad_tag_wrapped=" + std::to_string(m.chat_squad_tag_wrapped) +
			" rules_word_bits=" + rules_word +
			" | waypoint_goals_met_advance=" + std::to_string(r.waypoint_goals_met_advance) +
			" waypoint_advance_needs_done=" + std::to_string(r.waypoint_advance_needs_done) +
			" route_done_records=" + std::to_string(r.route_done_records) +
			" emplaced_turret_update=" + std::to_string(static_cast<int>(r.emplaced_turret_update)) +
			" emplaced_solo_planar_slew=" + std::to_string(r.emplaced_solo_planar_slew) +
			" narrow_still_player_radius=" + std::to_string(r.narrow_still_player_radius) +
			" spawn_kit=" + (r.spawn_kit == nullptr ? std::string("jo_single_player_page") : kit);
}

void bump_rule_row_per_game() {
	const std::string jo =
			"objective_checkpoint=0 spawn_zone_flag_always=0 armory_only_availability=2 "
			"join_state_fixed=0 loadout_class_fixed=0 lan_cd_key_check=0 chat_squad_tag_wrapped=0 "
			"rules_word_bits=0x0 | waypoint_goals_met_advance=1 waypoint_advance_needs_done=0 "
			"route_done_records=0 emplaced_turret_update=0 emplaced_solo_planar_slew=0 "
			"narrow_still_player_radius=0 spawn_kit=jo_single_player_page";
	const std::string dfx2 =
			"objective_checkpoint=1 spawn_zone_flag_always=1 armory_only_availability=3 "
			"join_state_fixed=1 loadout_class_fixed=1 lan_cd_key_check=1 chat_squad_tag_wrapped=1 "
			"rules_word_bits=0x10000 | waypoint_goals_met_advance=0 waypoint_advance_needs_done=1 "
			"route_done_records=1 emplaced_turret_update=1 emplaced_solo_planar_slew=3848520 "
			"narrow_still_player_radius=1 spawn_kit=WPN_M4AUTO,WPN_M4,WPN_MK23,WPN_AT4,"
			"WPN_GRENADEFB,WPN_GRENADEHE,WPN_GRENADESM,WPN_KNIFE";
	CHECK(bump_rule_row("jo") == jo);
	CHECK(bump_rule_row("dfx2") == dfx2);
	// OP: Acid Gambit's row is JO's, as it speaks JO's protocol at JO's protocol version.
	CHECK(bump_rule_row("acidgambit") == jo);
	CHECK(std::string(protocol_version(*title_protocol("acidgambit"))) ==
			protocol_version(*title_protocol("jo")));
	// The unwitnessed titles have no in-match protocol: their session part is refused by name.
	for (const char *code : { "jodemo", "dfx", "bhd" })
		CHECK(bump_rule_row(code) == "no in-match protocol");
}

// A version line's title parts (title_versions): the title's name where the line names no game,
// the protocol it speaks with its PV2 where its session part is witnessed; OP: Acid Gambit's is
// JO's; an unwitnessed protocol and an unknown title add no protocol.
void title_parts_of_the_version_line() {
	Versions jo;
	title_versions("jo", jo);
	CHECK(jo.game == "Joint Operations" && jo.protocol == "JO" && jo.protocol_version == "16");
	Versions dfx2;
	title_versions("DFX2", dfx2);
	CHECK(dfx2.game == "Delta Force Xtreme 2" && dfx2.protocol == "DFX2" &&
			dfx2.protocol_version == "16");
	Versions named;
	named.game = "My Game";
	title_versions("acidgambit", named);
	CHECK(named.game == "My Game" && named.protocol == "JO" && named.protocol_version == "16");
	Versions demo;
	title_versions("jodemo", demo);
	CHECK(demo.game == "Joint Operations (Demo)" && demo.protocol.empty() &&
			demo.protocol_version.empty());
	Versions unknown;
	title_versions("nosuchgame", unknown);
	title_versions("", unknown);
	CHECK(unknown.game.empty() && unknown.protocol.empty());
}

} // namespace

int main() {
	every_title_resolves();
	retail_protocols_complete();
	acidgambit_speaks_jo();
	unwitnessed_titles_refused();
	every_title_runs_rules();
	retail_rules_values();
	bump_rule_row_per_game();
	title_parts_of_the_version_line();
	if (g_failures == 0) {
		std::printf("games: all checks passed\n");
		return 0;
	}
	std::printf("games: %d check(s) failed\n", g_failures);
	return 1;
}
