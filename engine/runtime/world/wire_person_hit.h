// The client leg of the projectile damage pass over a wire-built person.
//
// A retail client builds every remote person (a remote player, a non-player
// infantry) as its own pool-0 entity from the spawn stream, its ItemDef's
// class callback included, so a round this client flies runs
// Projectile_ProcessDamageOnTarget over that entity exactly as over its own
// body: the friendly-fire gate, the hit record, the attacker stamp and the
// person class callback's event 1, with the in-session non-authority damage
// of zero [orig: Projectile_ProcessDamageOnTarget @0x4E7FB0;
// Weapon_CalcImpactDamage @0x4EC933..0x4EC93A; NapiNPClientMsg_0x00C ->
// Entity_InitFromItemDef @0x42E84D (the def's deathCallback)]. A joiner keeps
// those persons as replica rows (runtime/replication), so the leg reaches the
// row's fields through WirePersonBody: the joiner role loads the body from
// the row, the world runs the leg over it and the role stores it back, inside
// the round tick, in retail's order.
#pragma once

#include <cstdint>

namespace opennova::world {

// The client entity fields of one wire-built person that the damage pass and
// the person class callbacks read and write, by their retail offsets.
struct WirePersonBody {
	// --- read -----------------------------------------------------------------
	uint16_t wire_handle = 0xFFFF;
	// The ItemDef pointer (+0x20): a row whose type resolved to an items.def
	// row. A null def returns the damage pass at its head [orig: @0x4E7FCB].
	bool has_item_def = false;
	// ItemDef attrib (+84): its EWEAP bit 0x20 skips the attacker stamp
	// [orig: @0x4E81E3..0x4E81E7] and its NoDie bit keeps the damage under
	// Health [orig: @0x4E8069..0x4E8076].
	uint32_t item_attrib = 0;
	// The entity Flags word (+0x24): the spawn's word (Player 0x100) and the
	// client's own latches on it (the death edge's dead bit 2, mounted 0x40).
	uint32_t flags = 0;
	// The def's class callback row: plyr (Entity_HandleDamageAndTriggerZones
	// @0x407720) when its ai_function tag is "plyr", else org0 / org1
	// (OrganicClass_HandleEvent @0x407310) [orig: the rows @0x813048 /
	// @0x813018 / @0x813030 of g_EntityClassEventCallbackTable, matched by a
	// whole-string stricmp in Entity_LookupRenderCallbacks @0x407DE2].
	bool plyr_class = false;
	// The team byte (+354).
	uint8_t team = 0;
	// The AI slot's BERSERK bit (slot +4 & 0x200). The client allocates the
	// slot zeroed at the spawn and no client message writes the word, so it
	// reads clear [orig: NapiNPClientMsg_0x00C -> Entity_AllocateAISlot @0x42E81E,
	//  the memset @0x40D2E9].
	bool berserk = false;
	// commandGroup (+0x11C). No client message stores it: the 0x0C slot
	// memset leaves it 0 [orig: NapiNPClientMsg_0x00C @0x42E7F1].
	int16_t command_group = 0;
	// Position (+4..+0xC, 16.16) and Yaw (+0x10, BAM32).
	int32_t pos[3] = {};
	int32_t heading_bam = 0;
	// Health (+0x11E) <= 0: a dead record or an 0x13 zeroed it; an alive
	// player record floors it at 1.
	bool health_zero = false;

	// --- written --------------------------------------------------------------
	// The attacker stamp: lastAttacker (+0x178) takes the round's owner, here
	// by its wire identity, and +0x17C the round's ammo def (its table index)
	// [orig: @0x4E81F3 / @0x4E81F9].
	uint16_t last_attacker = 0xFFFF;
	int32_t last_attacker_ammo = -1;
	// The AI slot's alert byte (slot +0x88) [orig: OrganicClass_HandleEvent
	// @0x4073DB].
	uint8_t ai_alert = 0;
	// deathAnimStateId (+0x2C0), the current and pending body states
	// (+0x2BC / +0x2B8).
	int16_t death_anim_state = 0;
	int16_t anim_current = -1;
	int16_t anim_pending = 0;
	// The collision force's targets: the hit type (+0x368), moveTimer
	// (+0x148) and the velocity triple (+0x98 / +0x9C / +0xA0 slideDecay)
	// [orig: Entity_ApplyCollisionForce @0x4AF506 / @0x4AF50E..0x4AF541,
	//  the kicks @0x4AF5A9..0x4AF7DD].
	uint8_t hit_type = 0;
	int32_t move_timer = 0;
	int32_t vel[3] = {};
	// bodyRoll (+0x94) [orig: OrganicClass_HandleEvent @0x407564 / @0x407575].
	int32_t body_roll = 0;
	// Entity_OnDamageReceived's tail: wasHit (+0x36B), damageTimer (+0x2F8)
	// and its attacker (+0x2F4), here by wire identity [orig: @0x4AF866,
	// @0x4AF872, @0x4AF878].
	bool was_hit = false;
	int32_t damage_timer = 0;
	uint16_t damage_attacker = 0xFFFF;
	// spawnPhase, the think cadence the plyr callback re-arms
	// [orig: Entity_HandleDamageAndTriggerZones @0x407B5E / @0x407C71].
	int32_t spawn_phase = 0;
};

} // namespace opennova::world
