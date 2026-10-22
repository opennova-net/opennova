#pragma once

// The effect world's model-def list and the loaded-model page the C2S 0x3D answers from
// (D-NET-527). Retail keeps one node per model file ThreediGp_LoadModel created: the name
// up to its first '.' with ".3DI", found again case-insensitively, a file that does not
// load leaving no node, each new node inserted at the head. Game_StartMission empties the
// list at its teardown, its loads fill it in their order, and CEffectWorld_RebuildAllModel-
// Buffers freezes it: one dword per node not marked as foliage, head first, the page a
// joiner's 0x3D answers and the count a host's S2C 0x68 cursor wraps at.
// [orig: ThreediGp_LoadModel @0x5B6160 - the name @0x5b61a6..0x5b61d0, the find
//  CEffectDef_FindByTypeName @0x5b01a0 (the stricmp call @0x5B01B8), CEffectDef_Construct @0x5b01e0
//  (the head insert @0x5b0229 / @0x5b0232), the failed load's destroy @0x5b623f;
//  Game_StartMission @0x524360 - EffectWorld_DestroyAllAndInitDeviceCaps @0x524a6f;
//  CEffectDef_MarkFoliage @0x5b2220 (node+0x3D4); CEffectWorld_RebuildAllModelBuffers
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
namespace opennova::replication {
struct ClientState;
} // namespace opennova::replication

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
	// @0x601260 - ThreediGp_LoadModel @0x6012cb, CEffectDef_MarkFoliage @0x6012de].
	void load_foliage(const std::string &name);

	// The frozen page: one dword per unmarked node, head first (the reverse of creation).
	// Each is node dword 243 plus 244, which the rebuild's first pass leaves at 0 for every
	// node and no witnessed writer sets otherwise: zeros [orig: @0x5b13a0..0x5b1435]. The
	// 0x3D writer pages it a dword per row [orig: NetPacket_WriteEntityIndexList @0x42d950 -
	// the per-entry store @0x42d9f0, the 0x32 cap @0x42da7d].
	std::vector<uint32_t> freeze() const;
	// The files of the nodes the freeze pages (the unmarked ones), in load order: the models a
	// shell builds ahead of the mission's first frame, as each load builds its model and the
	// freeze's first pass every node's buffers, so a later swap to one (a death's husk) builds
	// nothing [orig: ThreediGp_LoadModel @0x5B6160 (ThreediGp_LoadFromFile @0x5b6230);
	// CEffectWorld_RebuildAllModelBuffers @0x5B3A80 (each node's CEffectDef_ResolveSubobjectMaterials
	// call @0x5B3A92)]. The
	// marked foliage nodes are the terrain's foliage pass's to build.
	std::vector<std::string> model_files() const;
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

// One pool-0 slot as the reload's pool-0 leg reads it: the item type id its spawn record named
// (the record's word, read signed), its Flags and its class [orig: NapiNPClientMsg_0x00C
// @0x42E730 - the signed word @0x42E840, the item index @0x42E84A, Flags @0x42E91A,
// playerClass @0x42E9D5;
// NapiNPClientMsg_FullEntitySpawn @0x433780 - the word stored zero-extended @0x433842 and
// read signed by the movsx @0x433B9A, Flags @0x433B69, playerClass @0x433BB5, the item index
// @0x433BD4].
struct PoolZeroBody {
	int type_id = 0;
	uint32_t flags = 0;
	uint8_t player_class = 0;
};

// A joiner's pool 0 in slot order, from its replica: the persons its stream's spawn records
// built (every S2C 0x0C, a 0x18), its own body among them. A retail joiner's mission start
// receives that stream inside NapiClient_WaitForGameStart, ahead of the reload, so the reload's
// pool-0 leg walks them [orig: Game_StartMission @0x524360 - NapiClient_WaitForGameStart
// @0x524E29 (its receive pumps @0x42CC7E..0x42CC92) ahead of Game_ReloadEntityModelsAndCallbacks
// @0x52550E]. A slot no spawn record filled (a row only a team or identity fold made) has type
// id 0, which the leg skips.
std::vector<PoolZeroBody> replica_pool0_bodies(const replication::ClientState &state);

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
	// A wire-header joiner's pool 0 (replica_pool0_bodies): the reload's pool-0 leg walks it
	// in place of the world's pool 0, which on such a joiner holds only its own body. Null:
	// the world's pool 0.
	const std::vector<PoolZeroBody> *pool0 = nullptr;
	// In a session the MP classes' camouflage items load; out of one, team 1's class's.
	bool in_session = true;
	uint8_t team1_class = 0;
	// A mission event with an action 39 of subtype 1 or 2 [orig: EventTrigger_HasActive-
	// SpecialEvent @0x452d90]. A joiner loads no mission body, so it has none.
	bool special_event = false;
	// The terrain's foliage defs load: on every host but a dedicated one (in session, the
	// authority, not a session peer), whose mission start jumps past the render setup that
	// is their only caller [orig: Game_StartMission @0x524360 - the flag @0x524942..0x524964,
	// the branch @0x5259AE..0x5259BF past Render_InitTerrainSubsystems @0x5259FD].
	bool foliage = true;
};

// Whether a mission event carries an action 39 of subtype 1 or 2: the special event whose
// items the mission start loads [orig: EventTrigger_HasActiveSpecialEvent @0x452d90 - each
// event's actions @0x452da8..0x452dd1, type 39 and subtype 1 / 2 @0x452dc7].
bool mission_has_special_event(const bms::File &mission);

// The list Game_StartMission builds, its loads in its order: the Avatars.def combos at the
// climate's variant, the ammo rows' tracer items, weapon.def's gfx1 / gfx3 models, the
// items the mission reloads (Game_ReloadEntityModelsAndCallbacks), the celestial models and
// upl.3di, hudfx.def's models, and (MissionStartModelSources::foliage) the terrain's foliage
// models, marked.
LoadedModelList build_mission_start_model_list(const MissionStartModelSources &sources);

// The page a mission start freezes for `kernel` over the mount `files`: the kernel's loaded
// tables and world, a load the kernel's asset store parsing the model (no renderer), the
// process's character-attribute table, and the mission's special event, an authority's alone
// (a joiner loads no mission body). Its size is the host's S2C 0x68 cursor wrap, its dwords a
// joiner's C2S 0x3D page. A listen host and a joiner freeze it on the full arm, a `dedicated`
// host (opennova-serve, the game's Serve Only host) on the reduced arm with no foliage leg
// before it [orig: Game_StartMission @0x524360 (the call @0x525A6E, the flag its third
// argument) -> Render_InitMissionTextures @0x587120 (the full arm's call @0x5871CF, the
// reduced arm's @0x58728C) -> CEffectWorld_RebuildAllModelBuffers @0x5B3A80].
std::vector<uint32_t> mission_start_model_page(const mission::MissionKernel &kernel,
		const mission::BootFileSource &files, const CharAttrTable *charattr, bool authority,
		bool dedicated);
// The list that page freezes, from the same sources: its model_files() are what the shell
// builds at the load, every listed item row's husk family among them [orig:
// EntityDef_LoadModelsAndCallbacks @0x439F50 - husk @0x43A0B6, the "xb" final husk @0x43A1C8,
// huskfinal @0x43A245 / @0x43A26E, huskshadow @0x43A38A]. A wire-header joiner passes its
// `replica`, whose pool 0 the reload walks (MissionStartModelSources::pool0).
LoadedModelList mission_start_model_list(const mission::MissionKernel &kernel,
		const mission::BootFileSource &files, const CharAttrTable *charattr, bool authority,
		bool dedicated, const replication::ClientState *replica = nullptr);

// One node of a frozen list as the model-list readout reports it (a debug instrument with no
// behaviour: the model_list_dump probe, Simulation::debug_mission_start_model_nodes): its
// file, its foliage mark, its place in load order (0 the first load) and its dword's place in
// the frozen page (-1 for a marked node, which the page skips). Retail's list is walked from
// g_ModelDefListHead, head first, the order the freeze reads it in [orig:
// CEffectWorld_RebuildAllModelBuffers @0x5B3A80 - the walk @0x5b3a89, the unmarked count
// @0x5b13b0..0x5b13d6, the values @0x5b142b; CEffectDef_FindByTypeName @0x5b01a0 - the node's
// name +0xFC @0x5B01B0, its next +0 @0x5B01C4].
struct LoadedModelListEntry {
	std::string name;
	bool foliage = false;
	uint32_t load_index = 0;
	int32_t page_index = -1;
};
// The list's entries head first (the reverse of load order).
std::vector<LoadedModelListEntry> loaded_model_list_entries(const LoadedModelList &list);

} // namespace opennova::inmatch
