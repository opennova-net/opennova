// The per-item ITEMS.DEF particlefx law (runtime/world/item_effects): the
// pool-walk attrib gates, the occupied-controller bypass, the identity
// aliases a vehicle_control_* event names an entity by, and the
// matched-userpoint / origin-fallback attach plan
// [orig: resolve_item_materials_and_spawn_bone_trails @ 0x522ee0 ->
//  ItemDef_GetBoneMaskByName @ 0x49ea40 -> Entity_SpawnBoneTrailEffect
//  @ 0x43bef0; the spawn_count==0 leg @ 0x43c097 -> @ 0x43c0a4].

#include <runtime/world/entity.h>
#include <runtime/world/item_effects.h>

#include <formats/def/def.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace opennova::def;
using namespace opennova::threedi;

namespace {
namespace w = opennova::world;

int failures = 0;

#define CHECK(condition)                                                        \
	do {                                                                          \
		if (!(condition)) {                                                          \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #condition);           \
			++failures;                                                                \
		}                                                                           \
	} while (0)

constexpr int kMarker = static_cast<int>(w::EntityKind::Marker);
constexpr int kItem = static_cast<int>(w::EntityKind::Item);
constexpr int kBuilding = static_cast<int>(w::EntityKind::Building);
constexpr int kOrganic = static_cast<int>(w::EntityKind::Organic);

void test_pool_gates_are_kind_sensitive() {
	// Pool 1 (items) skips attrib 0x42; pools 2/3 skip only the powerup bit;
	// organics (pool 0) and unknown kinds are never walked.
	CHECK(w::item_effect_pool_allows(kItem, 0));
	CHECK(w::item_effect_pool_allows(kItem, DEF_ITEM_ATTRIB_AIDATA));
	CHECK(!w::item_effect_pool_allows(kItem, DEF_ITEM_ATTRIB_POWERUP));
	CHECK(!w::item_effect_pool_allows(kItem, DEF_ITEM_ATTRIB_PLAYERCONTROL));
	CHECK(!w::item_effect_pool_allows(kItem,
			DEF_ITEM_ATTRIB_POWERUP | DEF_ITEM_ATTRIB_PLAYERCONTROL));
	CHECK(w::item_effect_pool_allows(kBuilding, 0));
	CHECK(w::item_effect_pool_allows(kBuilding, DEF_ITEM_ATTRIB_PLAYERCONTROL));
	CHECK(!w::item_effect_pool_allows(kBuilding, DEF_ITEM_ATTRIB_POWERUP));
	CHECK(w::item_effect_pool_allows(kMarker, 0));
	CHECK(w::item_effect_pool_allows(kMarker, DEF_ITEM_ATTRIB_PLAYERCONTROL));
	CHECK(!w::item_effect_pool_allows(kMarker, DEF_ITEM_ATTRIB_POWERUP));
	CHECK(!w::item_effect_pool_allows(kOrganic, 0));
	CHECK(!w::item_effect_pool_allows(-1, 0));
	CHECK(!w::item_effect_pool_allows(7, 0));
}

void test_controller_gate_bypasses_only_player_control() {
	CHECK(w::item_effect_controller_allows(kItem, DEF_ITEM_ATTRIB_PLAYERCONTROL));
	CHECK(w::item_effect_controller_allows(kItem,
			DEF_ITEM_ATTRIB_PLAYERCONTROL | DEF_ITEM_ATTRIB_AIDATA));
	CHECK(!w::item_effect_controller_allows(kItem, 0));
	CHECK(!w::item_effect_controller_allows(kItem,
			DEF_ITEM_ATTRIB_PLAYERCONTROL | DEF_ITEM_ATTRIB_POWERUP));
	CHECK(!w::item_effect_controller_allows(kBuilding, DEF_ITEM_ATTRIB_PLAYERCONTROL));
	CHECK(!w::item_effect_controller_allows(kMarker, DEF_ITEM_ATTRIB_PLAYERCONTROL));
	CHECK(!w::item_effect_controller_allows(kOrganic, DEF_ITEM_ATTRIB_PLAYERCONTROL));
	// Every attach admitted by the controller pass is one the pool walk
	// skipped, and vice versa: the two gates partition the PlayerControl bit.
	for (uint32_t attrib : {0u, DEF_ITEM_ATTRIB_POWERUP, DEF_ITEM_ATTRIB_PLAYERCONTROL,
			DEF_ITEM_ATTRIB_POWERUP | DEF_ITEM_ATTRIB_PLAYERCONTROL}) {
		CHECK(!(w::item_effect_pool_allows(kItem, attrib) &&
				w::item_effect_controller_allows(kItem, attrib)));
	}
}

std::vector<std::string> aliases(int32_t net_id, int32_t bms_id, int64_t spawn_origin,
		int32_t wire_handle) {
	std::vector<std::string> out;
	out.emplace_back("stale");
	w::item_effect_identity_aliases(net_id, bms_id, spawn_origin, wire_handle, out);
	return out;
}

bool same(const std::vector<std::string> &got, std::initializer_list<const char *> want) {
	if (got.size() != want.size()) return false;
	size_t i = 0;
	for (const char *expected : want) {
		if (got[i++] != expected) return false;
	}
	return true;
}

void test_identity_aliases() {
	// A placed record: net id, BMS id and spawn origin, no wire identity.
	const int64_t origin = w::spawn_origin_pack(static_cast<uint32_t>(kItem), 3);
	CHECK(same(aliases(71, 9001, origin, -1),
			{"net:71", "bms:9001", ("origin:" + std::to_string(origin)).c_str()}));
	// A wire-only synthetic attachment (bms 0, sentinel origin): the wire
	// alias alone, for both sentinel spellings.
	CHECK(same(aliases(0, 0, -1, 0x1004), {"wire:4100"}));
	CHECK(same(aliases(5, 0, static_cast<int64_t>(w::kSpawnOriginNone), 0x1004),
			{"wire:4100"}));
	// A wire node WITH an authored BMS id keeps its record aliases beside the
	// wire alias; the -1 origin contributes none.
	CHECK(same(aliases(0, 9001, -1, 0x1004), {"wire:4100", "bms:9001"}));
	// A simulation net id is not a wire handle: the event's a-word names
	// only the net alias.
	CHECK(same(aliases(77, 0, 0, -1), {"net:77"}));
	// 0xffff is the wire-handle none sentinel; zero is a valid handle.
	CHECK(same(aliases(5, 0, 0, 0xffff), {"net:5"}));
	CHECK(same(aliases(0, 0, -1, 0), {"wire:0"}));
	// Nothing to name.
	CHECK(aliases(0, 0, 0, -1).empty());
	CHECK(w::item_effect_aliases_intersect({"wire:4100", "bms:9001"}, {"bms:9001"}));
	CHECK(!w::item_effect_aliases_intersect({"wire:4100"}, {"wire:4101", "net:1"}));
	CHECK(!w::item_effect_aliases_intersect({}, {"net:1"}));
}

// A model whose first 16 userpoints carry the authored name at indices 0, 2
// and 9 (mixed case), plus a 17th and 18th copy past the scan limit.
struct SynthModel {
	std::vector<ThreediUserPoint> points;
	Threedi3di3 model = {};

	explicit SynthModel(size_t count) {
		points.resize(count);
		for (size_t i = 0; i < count; ++i) {
			std::snprintf(points[i].name, sizeof(points[i].name), "pt%02zu", i);
			points[i].x = static_cast<int32_t>(i) << 16;
		}
		model.user_points = points.data();
		model.user_point_count = count;
	}
	void name(size_t index, const char *value) {
		std::snprintf(points[index].name, sizeof(points[index].name), "%s", value);
	}
};

void test_attach_plan_matches_the_first_sixteen_or_falls_back_to_the_origin() {
	SynthModel synth(18);
	synth.name(0, "MFlash01");
	synth.name(2, "mflash01");
	synth.name(9, "MFLASH01");
	synth.name(16, "MFlash01");
	synth.name(17, "MFlash01");

	const w::ItemEffectAttachPlan matched = w::item_effect_attach_plan(synth.model, "MFlash01");
	CHECK(matched.mask == ((1u << 0) | (1u << 2) | (1u << 9)));
	CHECK(matched.user_points.size() == 3);
	CHECK(matched.user_points.size() == 3 && matched.user_points[0] == 0 &&
			matched.user_points[1] == 2 && matched.user_points[2] == 9);
	CHECK(!matched.origin_fallback);

	// Case-insensitive, exact: a prefix does not match.
	const w::ItemEffectAttachPlan prefix = w::item_effect_attach_plan(synth.model, "MFlash");
	CHECK(prefix.mask == 0 && prefix.user_points.empty() && prefix.origin_fallback);

	// An unmatched name, an empty name and no name all take the origin leg.
	CHECK(w::item_effect_attach_plan(synth.model, "nope").origin_fallback);
	CHECK(w::item_effect_attach_plan(synth.model, "").origin_fallback);
	CHECK(w::item_effect_attach_plan(synth.model, nullptr).origin_fallback);
	CHECK(w::item_effect_attach_plan(synth.model, "").user_points.empty());

	// A model with no userpoints at all: the origin leg.
	SynthModel bare(0);
	const w::ItemEffectAttachPlan none = w::item_effect_attach_plan(bare.model, "MFlash01");
	CHECK(none.mask == 0 && none.origin_fallback);

	// Only the first 16 are scanned: a name authored solely past the limit
	// never matches.
	SynthModel late(20);
	late.name(17, "FX00");
	const w::ItemEffectAttachPlan past = w::item_effect_attach_plan(late.model, "FX00");
	CHECK(past.mask == 0 && past.origin_fallback);
	late.name(15, "fx00");
	const w::ItemEffectAttachPlan edge = w::item_effect_attach_plan(late.model, "FX00");
	CHECK(edge.mask == (1u << 15) && edge.user_points.size() == 1 &&
			edge.user_points[0] == 15 && !edge.origin_fallback);
}

} // namespace

int main() {
	test_pool_gates_are_kind_sensitive();
	test_controller_gate_bypasses_only_player_control();
	test_identity_aliases();
	test_attach_plan_matches_the_first_sixteen_or_falls_back_to_the_origin();
	if (failures != 0) {
		std::printf("item_effects_test: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("item_effects_test passed\n");
	return 0;
}
