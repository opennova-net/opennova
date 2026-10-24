// The host's answer to a C2S 0x0F entity-info query (L4-s30, the x9 aikill OR cell's 0x18 for
// handle 0x0000): retail serializes the addressed pool row as its memory stands. A live row is
// its record; a row the destroy freed reads the destroy's tail (the zeroed block, its 0xFFFF
// mount words and its 0xFF at +0x154, the last occupant's aiState and animSlot bytes past the
// memset); a row no spawn took since the pool's clear reads all zeros. Pools past 1 and slots
// past the capacity draw no reply, nor does a requester without an added player.
// [orig: NapiNPServerMsg_HandlePlayerInfoRequest @0x514180 - the requester gates
//  @0x514191..0x5141A8, the pool and capacity gates @0x5141D2..0x5141F0, the row @0x514201;
//  NetPacket_SerializeObjectToBuffer @0x504D10 - mountHandles[8] / [9] @0x505105 /
//  @0x50511B, aiState @0x50518C, animSlot @0x5051A0, +0x154 @0x5051EB; Entity_Destroy
//  @0x43E810 - memset(entity, 0, 0x2B4) @0x43EA70, +0x154 = 0xFF @0x43EA78,
//  mountHandles[0..9] = 0xFFFF @0x43EA86..0x43EA92; Pool_Clear @0x442060]

#include <net/npwire/ingame_message_id.h>
#include <net/npwire/protocol_message.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_message_dispatch.h>
#include <runtime/inmatch/server_session.h> // set_connection_mode
#include <runtime/world/entity.h>
#include <runtime/world/world.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

using namespace opennova;

namespace {

int g_failures = 0;

void expect(bool cond, const char *what) {
	if (!cond) {
		std::fprintf(stderr, "FAIL: %s\n", what);
		++g_failures;
	}
}

inmatch::NapiNPConnection requester(inmatch::ConnectionPhase phase) {
	inmatch::NapiNPConnection c;
	c.type = inmatch::NapiNPConnection::kTypeServerSide;
	c.phase = phase;
	c.link.mode = replication::TransportMode::Client;
	c.admission_stage = inmatch::GameAdmissionStage::Complete;
	c.reply.player_slot = 1;
	c.reply.player_name = "ParityJoin";
	return c;
}

std::vector<ProtocolMessage> query(inmatch::NapiNPServerCtx &ctx, world::World &w, uint16_t handle) {
	std::vector<uint8_t> body = {static_cast<uint8_t>(handle & 0xFFu),
			static_cast<uint8_t>(handle >> 8)};
	inmatch::ServerDispatchInputs inputs;
	inputs.server_ctx = &ctx;
	return inmatch::dispatch_session_replies(ctx.config, ctx.np_protocol.connection_list[0],
			{make_protocol_message(c2s::ENTITY_INFO_QUERY, std::move(body))}, 5,
			ctx.np_protocol.connection_list, &w, inputs);
}

// The one S2C 0x18 body of the replies, or empty.
std::vector<uint8_t> reply_body(const std::vector<ProtocolMessage> &replies) {
	for (const ProtocolMessage &m : replies)
		if (m.tag == s2c::FULL_ENTITY_SPAWN) return m.payload;
	return {};
}

// The 49-byte record of a type-0 row, in the serializer's order: the slot id, the def's id
// and type, team, Flags, owner, the empty name, the three links, seatMask, the two fixed
// mount words, the position, Yaw / Pitch high words, aiState, animSlot, NetId, playerClass,
// the literal 0, +0x154, refNum, subType.
std::vector<uint8_t> type_zero_record(uint16_t slot, uint16_t mount_word, uint8_t ai_state,
		uint8_t anim_slot, uint8_t byte_154) {
	std::vector<uint8_t> b;
	b.push_back(static_cast<uint8_t>(slot & 0xFFu));
	b.push_back(static_cast<uint8_t>(slot >> 8));
	b.insert(b.end(), {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}); // id, type, team, Flags, owner, ""
	b.insert(b.end(), {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}); // occupant / ground / parent
	b.push_back(0);                                          // seatMask
	for (int i = 0; i < 2; ++i) {
		b.push_back(static_cast<uint8_t>(mount_word & 0xFFu));
		b.push_back(static_cast<uint8_t>(mount_word >> 8));
	}
	b.insert(b.end(), 16, 0); // the position words, Yaw and Pitch high words
	b.push_back(ai_state);
	b.push_back(anim_slot);
	b.insert(b.end(), {0, 0, 0, 0}); // NetId, playerClass, the literal 0
	b.push_back(byte_154);
	b.insert(b.end(), {0, 0}); // refNum, subType
	return b;
}

world::Entity soldier() {
	// 01TR's handle-0 soldier (Indonesian Soldier #1 with AK47 STANDING, 101413).
	world::Entity e;
	e.kind = world::EntityKind::Organic;
	e.has_item_def = true;
	e.item_id = 0x0585;
	e.item_type = 3;
	e.team = 2;
	e.health = 100;
	e.alive = true;
	e.ai_state = 0x0C;
	e.anim_slot = 9;
	return e;
}

struct Rig {
	inmatch::NapiNPServerCtx ctx;
	std::unique_ptr<world::World> w = std::make_unique<world::World>();
	Rig() {
		inmatch::set_connection_mode(ctx, inmatch::ConnectionMode::HostOnly);
		ctx.np_protocol.connection_list.push_back(requester(inmatch::ConnectionPhase::InMatch));
		for (int pool = 0; pool < 3; ++pool) w->registry.configure_pool(pool, 16);
	}
};

// The destroyed soldier's slot: type 0 with the destroy's stamps and the soldier's survivors.
void test_destroyed_row_reads_the_destroy_tail() {
	Rig r;
	const world::EntityHandle h = r.w->registry.spawn(0, soldier());
	expect(h.packed == 0x0000, "the soldier takes slot 0");
	const std::vector<uint8_t> live = reply_body(query(r.ctx, *r.w, 0x0000));
	expect(live.size() > 4 && live[2] == 0x85 && live[3] == 0x05 && live[4] == 3,
			"the live row answers its own record (type 0x0585, a person)");
	r.w->registry.despawn(h);
	const std::vector<uint8_t> freed = reply_body(query(r.ctx, *r.w, 0x0000));
	expect(freed == type_zero_record(0x0000, 0xFFFF, 0x0C, 9, 0xFF),
			"a destroyed row: 0xFFFF mount words, +0x154 0xFF, aiState / animSlot kept");
	// HeliLift's +0x219 clear leaves the rest of the tail.
	r.w->registry.clear_destroyed(h);
	expect(reply_body(query(r.ctx, *r.w, 0x0000)) == freed,
			"the +0x219 clear leaves the destroy's stamps");
	// The next occupant's spawn ends the tail; its own destroy writes a new one.
	world::Entity next = soldier();
	next.ai_state = 0x03;
	next.anim_slot = 4;
	const world::EntityHandle again = r.w->registry.spawn(0, next);
	expect(again == h, "the next spawn reuses slot 0");
	r.w->registry.despawn(again);
	expect(reply_body(query(r.ctx, *r.w, 0x0000)) == type_zero_record(0x0000, 0xFFFF, 0x03, 4, 0xFF),
			"the tail carries the last occupant's survivors");
}

// A row no spawn took since the pool's clear reads all zeros behind the null links.
void test_cleared_row_reads_zero() {
	Rig r;
	expect(reply_body(query(r.ctx, *r.w, 0x0005)) == type_zero_record(0x0005, 0x0000, 0, 0, 0),
			"a never-taken pool-0 row");
	expect(reply_body(query(r.ctx, *r.w, 0x100F)) == type_zero_record(0x100F, 0x0000, 0, 0, 0),
			"a never-taken pool-1 row");
	// The pool's clear (configure_pool) ends a destroy's tail too.
	r.w->registry.despawn(r.w->registry.spawn(0, soldier()));
	r.w->registry.configure_pool(0, 16);
	expect(reply_body(query(r.ctx, *r.w, 0x0000)) == type_zero_record(0x0000, 0x0000, 0, 0, 0),
			"the pool's clear zeroes the row");
}

// No reply past pool 1, past the capacity, or to a requester without an added player.
void test_gates() {
	Rig r;
	expect(query(r.ctx, *r.w, 0x2001).empty(), "pool 2: no reply (@0x5141F0)");
	expect(query(r.ctx, *r.w, 0x5000).empty(), "pool 5: no reply (@0x5141D2)");
	expect(query(r.ctx, *r.w, 0x0010).empty(), "slot 16 of 16: no reply");
	r.ctx.np_protocol.connection_list[0].phase = inmatch::ConnectionPhase::PendingSpawn;
	expect(query(r.ctx, *r.w, 0x0000).empty(), "no added player: no reply (@0x5141A8)");
}

} // namespace

int main() {
	test_destroyed_row_reads_the_destroy_tail();
	test_cleared_row_reads_zero();
	test_gates();
	if (g_failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("entity_info_query_reply: ok\n");
	return 0;
}
