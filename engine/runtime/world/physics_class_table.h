// The physics callback table: the row an items.def move_function binds, the
// per-frame update every entity of that item runs.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace opennova::world {

// The rows of the retail physics callback table, in table order (the value is
// the row index). Each row is {char name[8]; update callback}, 12 bytes. The
// table stores the 5-character names whole (`squib`, `ctank`, `cbike`) and
// `CHel` mixed-case; each comment names the row and its update callback.
// [orig: g_EntityClassPhysicsTable @0x82abc8, count dword_82AD60 = 34]
enum class PhysicsClass : uint8_t {
	Null,   // null   row @0x82abc8 -> @0x4a8070
	Envs,   // envs   row @0x82abd4 -> @0x4a8080
	Ewep,   // ewep   row @0x82abe0 -> @0x440ca0
	Ele0,   // ele0   row @0x82abec -> @0x4a20e0
	Door,   // door   row @0x82abf8 -> @0x4a91d0
	Towr,   // towr   row @0x82ac04 -> @0x4a8340
	Genx,   // genx   row @0x82ac10 -> @0x4a88b0
	Org0,   // org0   row @0x82ac1c -> @0x4aff60
	Org1,   // org1   row @0x82ac28 -> @0x4b9910
	Org2,   // org2   row @0x82ac34 -> @0x4b40e0
	Upfx,   // upfx   row @0x82ac40 -> @0x4a92e0
	Nade,   // nade   row @0x82ac4c -> @0x443f50
	Rock,   // rock   row @0x82ac58 -> @0x444a90
	Schl,   // schl   row @0x82ac64 -> @0x4482a0
	Clym,   // clym   row @0x82ac70 -> @0x4472f0
	Arti,   // arti   row @0x82ac7c -> @0x445500
	Squib,  // squib  row @0x82ac88 -> @0x448d50
	Chel,   // CHel   row @0x82ac94 -> @0x490310
	Cveh,   // cveh   row @0x82aca0 -> @0x48efc0
	Ctank,  // ctank  row @0x82acac -> @0x48f000
	Cbike,  // cbike  row @0x82acb8 -> @0x48eff0
	Cbot,   // cbot   row @0x82acc4 -> @0x48ef90
	Catv,   // catv   row @0x82acd0 -> @0x48f010
	Cpln,   // cpln   row @0x82acdc -> @0x45d6f0
	Ctrn,   // ctrn   row @0x82ace8 -> @0x48f060
	Chld,   // chld   row @0x82acf4 -> @0x45d540
	Aflr,   // aflr   row @0x82ad00 -> @0x443df0
	Gflr,   // gflr   row @0x82ad0c -> @0x443f40
	Rokt,   // rokt   row @0x82ad18 -> @0x443dd0
	Stng,   // stng   row @0x82ad24 -> @0x446060
	Hlfr,   // hlfr   row @0x82ad30 -> @0x446690
	Jvln,   // jvln   row @0x82ad3c -> @0x446ba0
	Arty,   // arty   row @0x82ad48 -> @0x447140
	Psec,   // psec   row @0x82ad54 -> @0x53be10
};

inline constexpr size_t kPhysicsClassRowCount = 34;

// The row's name as the table stores it.
const char *physics_class_row_name(PhysicsClass row);

// The row an items.def move_function binds: the row whose name equals the
// WHOLE token ignoring case. A name the table lacks, or none, binds row 0,
// null; a prefix never binds (`ctan`, `cbik`, `towr2`, `cvehicle` are null).
// [orig: EntityDef_InitAllCallbacks @0x4a5b2a..0x4a5b41 passes the whole
//  moveFunctionClass (ItemDef+0x150; "Null" when empty) to
//  EntityDef_LookupPhysicsCallback @0x4a9240, which stricmps it (@0x4a9262)
//  against each row and falls back to row 0 (@0x4a9272)]
PhysicsClass physics_class_from_move_function(std::string_view move_function);

// Whether the row's per-frame update keeps the entity's height as its record
// placed it: the eight rows whose update writes no position of the entity's
// own -- null (nullsub_2), envs (Entity_UpdateEnvSoundEmitter @0x4a8080), ewep
// (Entity_UpdateTransformAndTurret @0x440ca0, which writes its turret's
// transform, not its own), door (Entity_SetDefaultBoneCallbacks @0x4a91d0),
// genx (Entity_UpdateParentTransform @0x4a88b0: it moves only with the entity
// it stands on), upfx (Entity_UpdateWaterPhysicsAndEffects @0x4a92e0), org0
// (nullsub_28) and chld (nullsub_82). Every other row moves it (the vehicles'
// and aircraft's movers, the rounds', an elevator's fall and land, a toppling
// tower, the infantry bodies' gravity).
// [orig: g_EntityClassPhysicsTable @0x82abc8, each row's update as named]
bool physics_class_keeps_height(PhysicsClass row);

} // namespace opennova::world
