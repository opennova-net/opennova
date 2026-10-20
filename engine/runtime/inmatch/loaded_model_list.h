#pragma once

// The effect world's model-def list and the loaded-model page the C2S 0x3D answers from
// (D-NET-527). Retail keeps one node per model file ThreediGp_LoadModel created: the name
// up to its first '.' with ".3DI", found again case-insensitively, a file that does not
// load leaving no node, each new node inserted at the head. Game_StartMission empties the
// list at its teardown, its loads fill it in their order, and CEffectWorld_RebuildAllModel-
// Buffers freezes it: one dword per node not marked as foliage, head first, the page a
// joiner's 0x3D answers and the count a host's S2C 0x68 cursor wraps at.
// [orig: ThreediGp_LoadModel @0x5B6160 - the name @0x5b61a6..0x5b61d0, the find
//  CEffectDef_FindByTypeName @0x5b01a0 (stricmp @0x5b01c2), CEffectDef_Construct @0x5b01e0
//  (the head insert @0x5b0229 / @0x5b0232), the failed load's destroy @0x5b623f;
//  Game_StartMission @0x524360 - EffectWorld_DestroyAllAndInitDeviceCaps @0x524a6f;
//  CEffectWorld_MarkModelsDirty @0x5b2220 (node+0x3D4); CEffectWorld_RebuildAllModelBuffers
//  @0x5B3A80 - the count @0x5b13b0..0x5b13d6, the values @0x5b142b]

#include <runtime/inmatch/charattr_table.h>
#include <runtime/mission/runtime_boot.h>

#include <formats/def/def.h>
#include <formats/mission/bms.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace opennova::world {
struct AmmoTable;
class World;
} // namespace opennova::world
namespace opennova::mission {
class MissionKernel;
} // namespace opennova::mission

namespace opennova::inmatch {

class LoadedModelList {
public:
	// Whether a model file (its list name, "<stem>.3DI") loads: the mount holds it and it
	// parses [orig: ThreediGp_LoadFromFile @0x5b6230].
	using LoadsFn = std::function<bool(const std::string &file)>;

	explicit LoadedModelList(LoadsFn loads) : loads_(std::move(loads)) {}

	// The name a load files its node under: up to the first '.' (the whole name when it has
	// none) with ".3DI" [orig: @0x5b61a6..0x5b61d0].
	static std::string node_name(const std::string &name);

	// ThreediGp_LoadModel: the node `name` names exists after the call (found, or created
	// for a file that loads). False for a name whose file does not load.
	bool load(const std::string &name);
	// A foliage definition's load: the node, marked; the mark is the node's for good, so a
	// name another source also loads stays off the page [orig: Foliage_LoadDefAssets
	// @0x601260 - ThreediGp_LoadModel @0x6012cb, CEffectWorld_MarkModelsDirty @0x6012de].
	void load_foliage(const std::string &name);

	// The frozen page: one dword per unmarked node, head first (the reverse of creation).
	// Each is node dword 243 plus 244, which the rebuild's first pass leaves at 0 for every
	// node and no witnessed writer sets otherwise: zeros [orig: @0x5b13a0..0x5b1435]. The
	// 0x3D writer pages it a dword per row [orig: NetPacket_WriteEntityIndexList @0x42d950 -
	// the per-entry store @0x42d9f0, the 0x32 cap @0x42da7d].
	std::vector<uint32_t> freeze() const;
	// The nodes head first, each with its mark (tests and diagnostics).
	struct Node {
		std::string name;
		bool foliage = false;
	};
	std::vector<Node> nodes() const;

private:
	LoadsFn loads_;
	std::vector<Node> created_; // creation order; the list's head is the back
};

// What Game_StartMission loads models from between its teardown and the freeze.
struct MissionStartModelSources {
	// The mount: Avatars.def, hudfx.def and the mission's .trn and .env.
	const mission::BootFileSource *files = nullptr;
	// Whether a model file loads (LoadedModelList::LoadsFn).
	LoadedModelList::LoadsFn model_loads;
	// The mission header's climate and its terrain's and environment's base names.
	bms::ClimateType climate = bms::ClimateType::Desert;
	std::string terrain;
	std::string environment;
	// The tables the mission start loaded: weapon.def as parsed, items.def in row order, the
	// ammo table in row order, the charattr table.
	const def::DefWeaponsFile *weapons = nullptr;
	const def::DefItemsFile *items = nullptr;
	const world::AmmoTable *ammo = nullptr;
	const CharAttrTable *charattr = nullptr;
	// The world at the load (a joiner's: its world stream landed).
	const world::World *world = nullptr;
	// In a session the MP classes' camouflage items load; out of one, team 1's class's.
	bool in_session = true;
	uint8_t team1_class = 0;
	// A mission event with an action 39 of subtype 1 or 2 [orig: EventTrigger_HasActive-
	// SpecialEvent @0x452d90]. A joiner loads no mission body, so it has none.
	bool special_event = false;
};

// Whether a mission event carries an action 39 of subtype 1 or 2: the special event whose
// items the mission start loads [orig: EventTrigger_HasActiveSpecialEvent @0x452d90 - each
// event's actions @0x452da8..0x452dd1, type 39 and subtype 1 / 2 @0x452dc7].
bool mission_has_special_event(const bms::File &mission);

// The list Game_StartMission builds, its loads in its order: the Avatars.def combos at the
// climate's variant, the ammo rows' tracer items, weapon.def's gfx1 / gfx3 models, the
// items the mission reloads (Game_ReloadEntityModelsAndCallbacks), the celestial models and
// upl.3di, hudfx.def's models, and the terrain's foliage models, marked.
LoadedModelList build_mission_start_model_list(const MissionStartModelSources &sources);

// The page a mission start freezes for `kernel` over the mount `files`: the kernel's loaded
// tables and world, a load the kernel's asset store parsing the model (no renderer), the
// process's character-attribute table, and the mission's special event, an authority's alone
// (a joiner loads no mission body). Its size is the host's S2C 0x68 cursor wrap, its dwords a
// joiner's C2S 0x3D page; the game's host and joiner and opennova-serve's dedicated host build
// it alike [orig: Game_StartMission @0x524360 (the call @0x525A6E) ->
// Render_InitMissionTextures @0x587120 (the call @0x58728C) -> CEffectWorld_RebuildAllModelBuffers
// @0x5B3A80].
std::vector<uint32_t> mission_start_model_page(const mission::MissionKernel &kernel,
		const mission::BootFileSource &files, const CharAttrTable *charattr, bool authority);

} // namespace opennova::inmatch
