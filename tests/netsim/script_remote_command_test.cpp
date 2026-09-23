// The S2C 0x23 script remote command end to end: the host VM's flags-0x18 arm
// queues the typed record, the codec carries it byte for byte, and a joiner's
// replica fold hands it to the SAME handler body the host runs
// (wac::run_remote_command), so a targeted ptext/pwave prints or plays on the
// addressed joiner alone and a broadcast text prints on both endpoints.
// [orig: WacScript_ExecuteBytecode @0x4F58B0 — 0x18 arm @0x4f5ca5..0x4f5ee9;
//  GameMode_DispatchRemoteCommand @0x4F81E0 — `!is_authority` @0x4f8249,
//  zero-filled short body @0x4f8327..0x4f8393, flags gate @0x4f8429]
#include <cstdio>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <formats/wac/command.h>
#include <runtime/replication/client_replica_pipeline.h>
#include <runtime/wac/compiler.h>
#include <runtime/wac/remote_command.h>
#include <runtime/wac/vm.h>
#include <runtime/world/world.h>

using namespace opennova;
using namespace opennova::world;
using namespace opennova::wac;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

namespace {

struct Peer {
	std::unique_ptr<World> storage = std::make_unique<World>();
	World &world = *storage;
	Peer() {
		world.registry.configure_pool(0, 16);
		world.match.configure({});
	}
	EntityHandle spawn(uint16_t ssn, uint8_t slot, bool registered = true) {
		Entity entity;
		entity.net_id = ssn;
		entity.item_id = 1;
		entity.has_item_def = true;
		entity.health = 100;
		entity.alive = true;
		entity.engine_flags |= kEntityFlagPlayer; // the reserved humans group
		const auto handle = world.registry.spawn(0, entity);
		if (registered) world.match.upsert_player({handle, slot, "Player", {}});
		return handle;
	}
	int text_count(const std::string &text, int32_t a = 0) const {
		int count = 0;
		for (const Effect &e : world.out.effects.entries())
			if (e.kind == "text" && e.str == text && e.a == a) ++count;
		return count;
	}
	int kind_count(const char *kind) const { return world.out.effects.count(kind); }
};

// The host VM run: PLOOP over three humans (visited third, second, local) with
// a targeted ptext, then a broadcast text#, then a targeted pwave for one slot.
void run_host(World &world, const std::string &source) {
	const Program program = compile_source(source, {});
	CHECK(program.ok());
	WacVm vm;
	vm.load(program);
	vm.execute(world);
}

std::vector<uint8_t> to_wire(const world::ScriptRemoteCommand &record) {
	opennova::ScriptRemoteCommand wire;
	wire.command_index = record.command_index;
	for (const ScriptRemoteArg &arg : record.args)
		wire.args.push_back({static_cast<uint32_t>(arg.value), arg.text});
	return encode_script_remote_command(wire);
}

// The joiner role's application of one drained record (joiner_role.cpp).
void apply_on_joiner(World &world, const opennova::ScriptRemoteCommand &command) {
	std::vector<ScriptRemoteArg> args;
	for (const ScriptRemoteCommandArg &arg : command.args)
		args.push_back({static_cast<int32_t>(arg.value), arg.text});
	run_remote_command(world, command.command_index, args, {});
}

void test_host_records_reach_joiner_handlers() {
	Peer host;
	const EntityHandle local = host.spawn(10, 0);
	const EntityHandle second = host.spawn(11, 1);
	const EntityHandle third = host.spawn(12, 2);
	host.world.cached.local_player = local;

	Peer joiner;
	joiner.world.cached.local_player = joiner.spawn(11, 1);
	replication::ClientReplicaPipeline view; // a joiner: not the authority recipient
	CHECK(!view.authority_recipient());

	// Bare words reach the string pool upper-cased. [orig: Script_Compile @0x4F3418..0x4F341D]
	run_host(host.world,
			"ploop\nptext(hello)\nend\n"
			"text#(numbered,7)\n"
			"item=" + std::to_string(second.packed) + "\npwave(brief)\n");
	// The host printed only its own loop visit and the broadcast line; the
	// targeted pwave never played here.
	CHECK(host.text_count("HELLO") == 1);
	// text# formats "%s %i" in the handler on every peer [orig:
	// Chat_AddFormattedIntMessage @0x4EDB70 (the sprintf call @0x4EDB9E)].
	CHECK(host.text_count("NUMBERED 7") == 1);
	CHECK(host.kind_count("dialog_wav") == 0);
	const std::vector<world::ScriptRemoteCommand> &queue = host.world.out.script_remote_commands;
	CHECK(queue.size() == 4);
	if (queue.size() != 4) return;
	CHECK(queue[0].targeted && queue[0].target == third);
	CHECK(queue[1].targeted && queue[1].target == second);
	CHECK(!queue[2].targeted);
	CHECK(queue[3].targeted && queue[3].target == second);

	// Deliver what the server tick would send `second`: its targeted ptext,
	// the broadcast text#, and its targeted pwave.
	for (const size_t i : {size_t(1), size_t(2), size_t(3)})
		view.apply(s2c::SCRIPT_REMOTE_COMMAND, to_wire(queue[i]));
	CHECK(view.malformed_bodies() == 0);
	CHECK(view.unknown_tags() == 0);
	const std::vector<opennova::ScriptRemoteCommand> delivered =
			view.drain_script_remote_commands();
	CHECK(delivered.size() == 3);
	CHECK(view.drain_script_remote_commands().empty());
	for (const opennova::ScriptRemoteCommand &command : delivered) {
		CHECK(!command.read_error);
		apply_on_joiner(joiner.world, command);
	}
	CHECK(joiner.text_count("HELLO") == 1);
	CHECK(joiner.text_count("NUMBERED 7") == 1);
	CHECK(joiner.kind_count("dialog_wav") == 1);
	// The wire index is the shared handler's first row: text/wave, not
	// ptext/pwave. [orig: @0x4f5cb5..0x4f5cce]
	CHECK(delivered[0].command_index == wac_command_index("text"));
	CHECK(delivered[2].command_index == wac_command_index("wave"));
}

// A body that ends early zero-fills the remaining operands and still runs the
// handler; the authority endpoint never applies one; a row without the 0x18
// flags is rejected as malformed.
void test_short_body_zero_fill_and_authority_gate() {
	Peer joiner;
	replication::ClientReplicaPipeline view;
	std::vector<uint8_t> body;
	const uint16_t index = static_cast<uint16_t>(wac_command_index("text#"));
	body.push_back(uint8_t(index));
	body.push_back(uint8_t(index >> 8));
	for (const char ch : std::string("hi")) body.push_back(uint8_t(ch));
	body.push_back(0); // the Number operand is missing
	view.apply(s2c::SCRIPT_REMOTE_COMMAND, body);
	std::vector<opennova::ScriptRemoteCommand> delivered = view.drain_script_remote_commands();
	CHECK(delivered.size() == 1);
	if (delivered.size() == 1) {
		CHECK(delivered[0].read_error);
		CHECK(delivered[0].args.size() == 2 && delivered[0].args[0].text == "hi" &&
				delivered[0].args[1].value == 0);
		apply_on_joiner(joiner.world, delivered[0]);
		CHECK(joiner.text_count("hi 0") == 1);
	}

	replication::ClientReplicaPipeline authority;
	authority.set_authority_recipient(true);
	authority.apply(s2c::SCRIPT_REMOTE_COMMAND, body);
	CHECK(authority.drain_script_remote_commands().empty());
	CHECK(authority.malformed_bodies() == 0);

	std::vector<uint8_t> plain = {uint8_t(wac_command_index("elapse")), 0, 1, 0, 0, 0};
	view.apply(s2c::SCRIPT_REMOTE_COMMAND, plain);
	CHECK(view.drain_script_remote_commands().empty());
	CHECK(view.malformed_bodies() == 1);
}

} // namespace

int main() {
	test_host_records_reach_joiner_handlers();
	test_short_body_zero_fill_and_authority_gate();
	std::printf("script_remote_command: %d failures\n", failures);
	return failures ? 1 : 0;
}
