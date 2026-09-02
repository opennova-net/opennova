class_name EffectSpawnOptions
extends RefCounted
## The game-level half of one effect spawn: the admission / binding / render
## domain policy, the slot and owner KEYS EffectWorld interns to native
## tokens, the owner pose it seeds around the spawn, and the tick provenance.
## EffectWorld composes the native EffectSpawnRequest from this plus the
## interned effect handle; the request's remaining fields (tint, spring,
## LOD divisor, kill plane) keep their native defaults.

var admission: int = EffectScene.ADMISSION_ALWAYS
var binding: int = EffectScene.BINDING_WORLD
var render_domain: int = EffectScene.RENDER_DOMAIN_WORLD
## The slot identity the owned admission policies key on; null = no slot.
var slot_key: Variant = null
## The follow owner for BINDING_FOLLOW_OWNER; null = no owner.
var owner_key: Variant = null
## The owner pose seeded before the spawn (after it for ReplaceOwned, so the
## predecessor detaches at its own last pose) when `has_owner_transform`.
var owner_transform := Transform3D.IDENTITY
var has_owner_transform := false
var owner_relative_transform := Transform3D.IDENTITY
var initial_age_ticks := 0
var source_tick := 0
var source_order := 0
