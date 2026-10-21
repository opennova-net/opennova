// D-NET-348 (FOUND 3): a player body takes its bound items.def row's class-table row, by the
// row's ai_function: the class init the spawn calls (def+0x148) and the class event callback it
// copies into entity+0x1C8 (def+0x138), which the player-body update's think, the damage sites
// and the drowning call. "plyr" runs PlayerClass_InitEntity (the equipped default, the local
// lock tone) and Entity_HandleDamageAndTriggerZones; "org0" / "org1" Entity_InitOrganicAI (no
// equipped store: the memset's 0) and OrganicClass_HandleEvent (event 0 re-arms the think at
// 62, event 4 zeroes the health word, no dead return); the null row (an empty tag, a name no
// row carries) no init and NullClass_HandleEvent (+0x2AC = 0x1000000); "brrl" no init and
// Entity_HandleDeathOnAuthority (the authority's 1920 re-arm, the dying body's dead bit and 10).
// [orig: g_EntityClassEventCallbackTable @0x813000 (null @0x813000, org0 @0x813018, org1
//  @0x813030, plyr @0x813048, brrl @0x813060); Entity_LookupRenderCallbacks @0x407DC0 - the
//  stricmp @0x407DE2, the row-0 default @0x407DEE; Entity_SpawnFromAnimSlotProperty @0x43C390 -
//  the init @0x43C4F0..0x43C4FE, the event copy @0x43C530..0x43C536; PlayerClass_InitEntity
//  @0x4B1060 - @0x4B10D0..0x4B10DE, @0x4B1100..0x4B1116; OrganicClass_HandleEvent @0x407310 -
//  @0x40732D, @0x407343; NullClass_HandleEvent @0x406FF0 (@0x406FF4); Entity_HandleDeathOnAuthority
//  @0x407CC0 - @0x407D2C..0x407D47; Entity_HandleDamageAndTriggerZones @0x407720 - the dead return
//  @0x40772F, the re-arm @0x407B5E]
// Every items.def / charattr.def row is the test's own; core-only, heap-held worlds.

#include <formats/def/def.h>
#include <runtime/inmatch/charattr_table.h>
#include <runtime/mission/item_traits.h>
#include <runtime/world/ai.h>
#include <runtime/world/entity_commands.h>
#include <runtime/world/infantry.h>
#include <runtime/world/local_player.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/world.h>

#include <cstdint>
#include <cstdio>
#include <memory>

using namespace opennova;
namespace w = opennova::world;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

namespace {

// Row 0 the "Null" marker; then a plyr person (5305), an org0 person (5311), a person with no
// ai_function (5320: the null row), a brrl person (5321) and an ewep person (5322).
const char kItems[] =
		"begin \"Null\"\r\n id 100000\r\n type marker\r\nend\r\n"
		"begin \"Player\"\r\n id 105305\r\n type person\r\n hp 150\r\n ai_function plyr\r\nend\r\n"
		"begin \"Soldier\"\r\n id 105311\r\n type person\r\n hp 100\r\n ai_function org0\r\nend\r\n"
		"begin \"Classless\"\r\n id 105320\r\n type person\r\n hp 100\r\nend\r\n"
		"begin \"Barrel man\"\r\n id 105321\r\n type person\r\n hp 100\r\n ai_function brrl\r\nend\r\n"
		"begin \"Gun man\"\r\n id 105322\r\n type person\r\n hp 100\r\n ai_function ewep\r\nend\r\n";

// Class 5 the plyr row, 6 the org0 row, 7 the classless row, 8 the brrl row, 9 the ewep row.
const char kCharattr[] =
		"[CHARACTER5]\r\nJUNGLE_CAMMO\t= 5305\r\nDESERT_CAMMO\t= 5305\r\nARCTIC_CAMMO\t= 5305\r\n"
		"[CHARACTER6]\r\nJUNGLE_CAMMO\t= 5311\r\nDESERT_CAMMO\t= 5311\r\nARCTIC_CAMMO\t= 5311\r\n"
		"[CHARACTER7]\r\nJUNGLE_CAMMO\t= 5320\r\nDESERT_CAMMO\t= 5320\r\nARCTIC_CAMMO\t= 5320\r\n"
		"[CHARACTER8]\r\nJUNGLE_CAMMO\t= 5321\r\nDESERT_CAMMO\t= 5321\r\nARCTIC_CAMMO\t= 5321\r\n"
		"[CHARACTER9]\r\nJUNGLE_CAMMO\t= 5322\r\nDESERT_CAMMO\t= 5322\r\nARCTIC_CAMMO\t= 5322\r\n";

constexpr uint8_t kPlyr = 5, kOrg = 6, kNull = 7, kBrrl = 8, kEwep = 9;
constexpr uint8_t kEquipped = 3;

struct Rig {
	std::unique_ptr<w::World> world = std::make_unique<w::World>();
	std::unique_ptr<w::LocalPlayer> local;
	def::DefItemsFile items{};
	uint16_t next_net_id = 0xFF00;

	Rig() {
		CHECK(def::def_parse_items_memory(reinterpret_cast<const uint8_t *>(kItems),
				sizeof(kItems) - 1, &items) == 0);
		world->registry.configure_pool(0, 8);
		world->rules.logic_authority = true;
		mission::resolve_item_traits(*world, items, {}, nullptr);
		inmatch::CharAttrTable table;
		inmatch::charattr_load(table, reinterpret_cast<const uint8_t *>(kCharattr),
				sizeof(kCharattr) - 1);
		CHECK(table.loaded);
		inmatch::install_class_attribute_flags(*world, nullptr, table);
		local = std::make_unique<w::LocalPlayer>(*world);
		world->local_player_state = local.get();
	}
	~Rig() {
		world->local_player_state = nullptr;
		def::def_free_items(&items);
	}

	// The host's own body of `player_class`, its lock tone raised ahead of the spawn.
	w::Entity *spawn_local(uint8_t player_class) {
		local->radar.lock_tone = 7;
		w::PlayerSpawn s;
		s.player_class = player_class;
		s.net_id = next_net_id++;
		s.equipped_adm_index = kEquipped;
		return world->registry.get(w::spawn_player(*world, s));
	}
	w::Entity *spawn_remote(uint8_t player_class) {
		w::PlayerSpawn s;
		s.player_class = player_class;
		s.net_id = next_net_id++;
		s.equipped_adm_index = kEquipped;
		return world->registry.get(w::spawn_remote_player(*world, s));
	}
	w::PlayerBodyClass row_class(const w::Entity &body) const {
		return w::player_item_row(*world, body).body_class;
	}
};

// The table walk: the whole name ignoring case; an empty tag and a miss read the null row.
void test_the_tag_names_its_row() {
	CHECK(w::player_body_class_from_tag("plyr") == w::PlayerBodyClass::kPlayer);
	CHECK(w::player_body_class_from_tag("PLYR") == w::PlayerBodyClass::kPlayer);
	CHECK(w::player_body_class_from_tag("org0") == w::PlayerBodyClass::kOrganic);
	CHECK(w::player_body_class_from_tag("Org1") == w::PlayerBodyClass::kOrganic);
	CHECK(w::player_body_class_from_tag("brrl") == w::PlayerBodyClass::kBarrel);
	CHECK(w::player_body_class_from_tag("null") == w::PlayerBodyClass::kNull);
	CHECK(w::player_body_class_from_tag("psec") == w::PlayerBodyClass::kNull);
	CHECK(w::player_body_class_from_tag("") == w::PlayerBodyClass::kNull);
	CHECK(w::player_body_class_from_tag("plyr2") == w::PlayerBodyClass::kNull);
	CHECK(w::player_body_class_from_tag("chel") == w::PlayerBodyClass::kOther);
	CHECK(w::player_body_class_from_tag("ewep") == w::PlayerBodyClass::kOther);
}

// The spawn's class init by the bound row: the player class stores the equipped default and
// resets the local body's lock tone; the organic, null and barrel rows store neither (the
// equipped byte the plyr serializer echoes stays the memset's 0).
void test_the_spawn_runs_its_rows_init() {
	struct Case {
		uint8_t player_class;
		w::PlayerBodyClass cls;
		uint8_t equipped;
		int32_t lock_tone;
	};
	for (const Case c : {Case{kPlyr, w::PlayerBodyClass::kPlayer, kEquipped, 0},
				Case{kOrg, w::PlayerBodyClass::kOrganic, 0, 7},
				Case{kNull, w::PlayerBodyClass::kNull, 0, 7},
				Case{kBrrl, w::PlayerBodyClass::kBarrel, 0, 7},
				Case{kEwep, w::PlayerBodyClass::kOther, kEquipped, 0}}) {
		Rig r;
		w::Entity *body = r.spawn_local(c.player_class);
		CHECK(body != nullptr);
		if (body == nullptr) continue;
		CHECK(r.row_class(*body) == c.cls);
		CHECK(body->equipped_adm_index == c.equipped);
		CHECK(r.local->radar.lock_tone == c.lock_tone);
		// Every class's body is the local player (the mission start's bind).
		CHECK(r.world->cached.local_player == body->handle);
		// A remote body's equipped byte follows the row the same way.
		const w::Entity *remote = r.spawn_remote(c.player_class);
		CHECK(remote != nullptr && remote->equipped_adm_index == c.equipped);
	}
}

// The think (event 0) by the bound row: plyr re-arms at 64 (and a dead body returns), org at
// 62, the null row at 0x1000000, the barrel row at 1920 for a live body on the authority.
void test_the_think_runs_its_rows_callback() {
	Rig r;
	w::Entity *plyr = r.spawn_remote(kPlyr);
	w::Entity *org = r.spawn_remote(kOrg);
	w::Entity *null_row = r.spawn_remote(kNull);
	w::Entity *brrl = r.spawn_remote(kBrrl);
	CHECK(plyr != nullptr && org != nullptr && null_row != nullptr && brrl != nullptr);
	if (plyr == nullptr || org == nullptr || null_row == nullptr || brrl == nullptr) return;
	w::player_body_row_event(*r.world, *plyr, 0);
	CHECK(plyr->spawn_phase == 64);
	w::player_body_row_event(*r.world, *org, 0);
	CHECK(org->spawn_phase == 62);
	w::player_body_row_event(*r.world, *null_row, 0);
	CHECK(null_row->spawn_phase == 0x1000000);
	w::player_body_row_event(*r.world, *brrl, 0);
	CHECK(brrl->spawn_phase == 1920);
	// A dead plyr body returns at the head; a dead org body still re-arms (no dead return).
	plyr->spawn_phase = -5;
	plyr->engine_flags |= w::kEntityFlagDead;
	w::player_body_row_event(*r.world, *plyr, 0);
	CHECK(plyr->spawn_phase == -5);
	org->spawn_phase = -5;
	org->engine_flags |= w::kEntityFlagDead;
	w::player_body_row_event(*r.world, *org, 0);
	CHECK(org->spawn_phase == 62);
}

// The barrel row's authority think on a body at or below 0 health: the dead bit and 10; off
// the authority nothing.
void test_the_barrel_rows_dying_body() {
	Rig r;
	w::Entity *brrl = r.spawn_remote(kBrrl);
	CHECK(brrl != nullptr);
	if (brrl == nullptr) return;
	brrl->health = 0;
	w::player_body_row_event(*r.world, *brrl, 0);
	CHECK((brrl->engine_flags & w::kEntityFlagDead) != 0 && brrl->spawn_phase == 10);
	Rig client;
	client.world->rules.logic_authority = false;
	w::Entity *other = client.spawn_remote(kBrrl);
	CHECK(other != nullptr);
	if (other == nullptr) return;
	other->spawn_phase = -1;
	w::player_body_row_event(*client.world, *other, 0);
	CHECK(other->spawn_phase == -1);
}

// A damage site's event 4 (the class callback a kill fires) by the row: the organic class
// zeroes the health word; the plyr class keeps it (its event-4 arm is the authority's seat
// leave, then the health tail).
void test_a_kill_event_runs_its_rows_callback() {
	Rig r;
	w::Entity *plyr = r.spawn_remote(kPlyr);
	w::Entity *org = r.spawn_remote(kOrg);
	CHECK(plyr != nullptr && org != nullptr);
	if (plyr == nullptr || org == nullptr) return;
	const int32_t plyr_health = plyr->health;
	w::hit_record_class_event(*r.world, *plyr, 4);
	CHECK(plyr->health == plyr_health);
	w::hit_record_class_event(*r.world, *org, 4);
	CHECK(org->health == 0);
}

} // namespace

int main() {
	test_the_tag_names_its_row();
	test_the_spawn_runs_its_rows_init();
	test_the_think_runs_its_rows_callback();
	test_the_barrel_rows_dying_body();
	test_a_kill_event_runs_its_rows_callback();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("PASS npruntime_player_body_class\n");
	return 0;
}
