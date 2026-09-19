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
#include <runtime/assets/asset_store.h>
#include <runtime/world/world.h>

#include <cstdint>
#include <functional>

namespace opennova::mission {


void resolve_minefields(world::World &world, const def::DefItemsFile &items,
                        const assets::AssetStore &models);

// The §5.10b wire-dispatch class supplier: maps an items.def definition id to
// the wire entity-class BYTE (npwire EntityClass values; 0 = Unknown, the
// fail-closed default for missing/ambiguous definitions). Injected by the
// embedder so the ONE wire-class source stays the netsim
// ItemReplicationCatalog (ADR 0026) — mission code never links the net stack.
using ItemWireClassFn = std::function<uint8_t(int definition_id)>;

// Stamp every live registry entity's items.def-derived traits (AI-capable
// gate, wire class byte, healthMax lift, armor/damage-reduction, AS zone
// attribs, corpse timing), fill the world's per-item death-trait and
// vehicle-trait tables, rebuild the throwable class bindings, and build+latch
// the AS zone-slot chain. Duplicate definition ids resolve last-wins, the
// same load-order overwrite the binding's id-keyed item map exposed.
// Idempotent; call after mission promotion (and again after spawning the
// local player). A valid only handle initializes one new row without rebuilding
// mission capture state; the default performs the original full load sweep.
void resolve_item_traits(world::World &world, const opennova::def::DefItemsFile &items,
                         const ItemWireClassFn &wire_class, world::EntityHandle only = {});

// Rebind item SHOT slots after loading the bank/profile catalogs.
void resolve_item_event_sounds(world::World &world, const def::DefItemsFile &items);

// Resolve the organic's four ammo bytes and three launch userpoints, seed its
// clipsize magazine, and bind body sound profiles and SM weapon ammo. Call after
// mission tables/models are available, or with only for a newly spawned body.
// Ammo lookup misses and unresolved userpoints store zero; an absent ammo key
// preserves the initial byte. Returns the number of armed bodies/SM weapon blocks.
// [orig: Entity_InitOrganicAI @ 0x4BFCC0]
int resolve_ai_weapons(world::World &world, const opennova::def::DefItemsFile &items,
                       world::EntityHandle only = {}, const assets::AssetStore *models = nullptr);

} // namespace opennova::mission
