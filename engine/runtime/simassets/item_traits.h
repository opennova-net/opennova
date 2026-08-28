// The items.def trait fold (ADR 0028): the simulation stamps its entity/world
// traits by reading DefItemDef rows directly from the embedder's retained
// items.def parse — the def fields Entity_InitFromItemDef and the load-time
// callback resolve consume [orig: Entity_InitFromItemDef @ 0x49e550;
// EntityDef_InitAllCallbacks @ 0x4a5a70; ItemDef_ParsePhysicsProperty
// @ 0x49d870]. Moved verbatim from the shell binding's item-database sweep;
// the getter surface it replaced was a field-for-field projection of these
// same rows, so a direct read is the identical contract minus the Dictionary
// re-pack.
#pragma once

#include <formats/def/def.h>
#include <runtime/world/world.h>

#include <cstdint>
#include <functional>

namespace opennova::simassets {

// The §5.10b wire-dispatch class supplier: maps an items.def definition id to
// the wire entity-class BYTE (npwire EntityClass values; 0 = Unknown, the
// fail-closed default for missing/ambiguous definitions). Injected by the
// embedder so the ONE wire-class source stays the netsim
// ItemReplicationCatalog (ADR 0026) — simassets never links the net stack.
using ItemWireClassFn = std::function<uint8_t(int definition_id)>;

// Stamp every live registry entity's items.def-derived traits (AI-capable
// gate, wire class byte, healthMax lift, armor/damage-reduction, AS zone
// attribs, corpse timing), fill the world's per-item death-trait and
// vehicle-trait tables, rebuild the throwable class bindings, and build+latch
// the AS zone-slot chain. Duplicate definition ids resolve last-wins, the
// same load-order overwrite the binding's id-keyed item map exposed.
// Idempotent; call after mission promotion (and again after spawning the
// local player).
void resolve_item_traits(world::World &world, const DefItemsFile &items,
                         const ItemWireClassFn &wire_class);

// The D-AI-5 host weapon seed + per-body sound-profile bind: stamp each AI
// entity's anim-fire round (items.def ammo_closeattack resolved against the
// loaded mission ammo table) and clipsize magazine reseed, and bind its
// items.def sound_profile against the loaded profile table. Returns the
// armed-NPC count. Call AFTER the ammo table is loaded; an unresolved or
// absent round name leaves the NPC unarmed.
int resolve_ai_weapons(world::World &world, const DefItemsFile &items);

} // namespace opennova::simassets
