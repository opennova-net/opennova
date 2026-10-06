#pragma once

#include <stddef.h>

namespace opennova::def {

/* The items.def rows the engine fixes (docs/world/itemdef-re.md, "The ids and rows the engine fixes"):
 * the ids its code looks a row up by, and the two rows it treats by their place. A project of its own
 * that retail Jointops.exe must run keeps each id for what the engine uses it for. A row's `type` is
 * the runtime id (items.def `id` less 100000, what a mission record and the wire carry). */

/* What an authoring tool does with a record that holds a reserved id but is not of the reserved
 * kind, or with a reserved record whose id moves away:
 *  - Refuse: a place or an objective the engine finds or makes by its id (a start, a waypoint, a
 *    teleport target, a map marker, the KOTH centre, a flag or a flag bay). A record's TYPE decides the
 *    pool a mission places it in, and the engine walks one pool for it: a record of another type there
 *    is never what the engine looks for, in any mode.
 *  - Warn: a model or an actor the engine takes from the row by its id (the parachute, the goggles,
 *    the binoculars, the heli lift's helicopter and medics, the palm fragment, an id only preloaded,
 *    the minimap's emplacement icon, the teammate's remapped type). A project without the row loses
 *    only that model or actor: another use of the id is allowed, and flagged. */
enum class ReservedItemRule { Refuse, Warn };

struct ReservedItem {
	int type;               /* the runtime type id */
	int kind;               /* the items.def `type` (DefItemType) the engine expects; 0: any */
	ReservedItemRule rule;
	const char *label;      /* what it is, in a few words ("Co-op start") */
	const char *retail;     /* retail's own items.def row name at the id; "" where retail ships none */
	const char *use;        /* what the engine does with it, a sentence */
};

/* items.def's id of a runtime type: the `id` arm subtracts 100000 before it stores the type
 * [orig: ItemDef_ParseProperty @ 0x49eb00, `sub eax, 186A0h` @0x49EC54]. */
inline constexpr int DEF_ITEM_ID_BASE = 100000;

/* Every reserved row, in type order. */
const ReservedItem *reserved_items(size_t *count);
/* The reserved row of a runtime type (null: none). */
const ReservedItem *reserved_item_by_type(int type);
/* The reserved row of an items.def id (null: none). */
const ReservedItem *reserved_item_by_id(int items_def_id);
/* Whether a record of DefItemType `item_type` is what the engine expects at the row's id. */
bool reserved_item_kind_matches(const ReservedItem &row, int item_type);

/* The row every lookup that finds no id resolves to: ItemList_FindIndexByTypeId returns 0 on a miss
 * [orig: ItemList_FindIndexByTypeId @ 0x49e100, the scan @0x49E120..0x49E122], so a type the file
 * lacks becomes row 0, and so does a player of a class with no camo item [orig:
 * Entity_SpawnFromAnimSlotProperty @ 0x43c390, the lookup @0x43C3CA]; and an entity of row 0 is no
 * item to the pool walks and the script predicates, which test ItemTypeIndex != 0 [orig:
 * Entity_UpdateVehiclePhysics @ 0x48BDD9; Entity_IsOnTopOfChain @ 0x4F1A35]. Retail's is the "Null"
 * marker, which no mission places. */
inline constexpr size_t DEF_ITEMS_FALLBACK_ROW = 0;
/* The row whose hulls the boats and the aircraft brake for: their avoid brakes compare a
 * neighbour's ItemTypeIndex with 1 [orig: Entity_UpdateWatercraftPhysics @ 0x48E5AA;
 * Entity_ProcessAirVehiclePhysics @ 0x470B08; Entity_UpdateAircraftPhysics @ 0x4919D1]. */
inline constexpr size_t DEF_ITEMS_BRAKE_ROW = 1;

} // namespace opennova::def
