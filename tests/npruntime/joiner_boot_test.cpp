// The one joiner bring-up (inmatch/joiner_boot.h), headless: a JoinerRole booted over an in-memory
// socket with no Godot, its connect exchange driven against a scripted host's ServerHello, as the
// game's Simulation::enable_join boots it.
//   - boot_joiner leaves the role dialing the socket's host with a runtime that took every setting
//     and table: the charattr.def table (and the World's per-class words from it), the side
//     profile, the JOIN checksum's root, the load seams; the hello leaves at the first poll, and
//     the ClientAuth past the ServerHello carries the settings' APPID;
//   - a runtime a load rebuilt (JoinerRole::bring_up with no hello out) takes the same through
//     install_joiner_runtime, its APPID included (before, a rebuild dropped the APPID, the CD
//     cookie, the GLSVSS request, the browse row and the connect log the dial had installed);
//   - settle_streamed_types runs a header-only join's model-derived legs in their order over the
//     stream's types in first-seen order.
#include <runtime/inmatch/joiner_boot.h>

#include <net/npwire/nw_session_framing.h>
#include <net/npwire/session_hello.h>
#include <net/npwire/session_keys.h>
#include <net/protocol/protocol.h>
#include <runtime/inmatch/charattr_table.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/join_character_profile.h>
#include <runtime/inmatch/joiner_role.h>
#include <runtime/mission/mission_kernel.h>
#include <runtime/world/present_rows.h>
#include <runtime/world/title_rules.h>
#include <runtime/world/world.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace opennova;
namespace inmatch = opennova::inmatch;

namespace {

int failures = 0;

bool check(bool ok, const char *what) {
	if (!ok) {
		std::fprintf(stderr, "FAIL: %s\n", what);
		++failures;
	}
	return ok;
}

constexpr PeerAddr kHost{ 0x0200000Au, 9200 };
constexpr PeerAddr kLocal{ 0x0300000Au, 51234 };

// The joiner's socket: what it sends to the host, and an inbox the test fills as the host.
class ScriptedSocket final : public IDatagramSocket {
public:
	std::vector<std::vector<uint8_t>> sent;
	std::deque<std::vector<uint8_t>> inbox;
	int recv_from(uint8_t *buf, std::size_t cap, PeerAddr &from) override {
		if (inbox.empty())
			return 0;
		std::vector<uint8_t> datagram = std::move(inbox.front());
		inbox.pop_front();
		if (datagram.size() > cap)
			return -1;
		from = kHost;
		std::memcpy(buf, datagram.data(), datagram.size());
		return static_cast<int>(datagram.size());
	}
	void send_to(const PeerAddr &to, const uint8_t *data, std::size_t size) override {
		if (to == kHost)
			sent.emplace_back(data, data + size);
	}
};

bool opcode_of(const std::vector<uint8_t> &datagram, uint8_t &opcode, std::vector<uint8_t> &body) {
	return nw_decode_inbound(datagram.data(), datagram.size(), opcode, body);
}

// The join's connect exchange as far as its ClientAuth: the first poll's ClientHello, the host's
// ServerHello into the socket, then polls until the 0x42 leaves (its connect state starts a
// millisecond past the 0x81's receive), bounded at two seconds. The ClientAuth's CU APPID, "" when
// it carries none or never left.
std::string client_auth_app_id(inmatch::JoinerRole &role, ScriptedSocket &socket) {
	role.poll_preload();
	ClientHello hello;
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	if (!check(!socket.sent.empty() && opcode_of(socket.sent.back(), opcode, body) &&
						opcode == SESSION_OPCODE_CLIENT_HELLO &&
						parse_client_hello(body.data(), body.size(), hello),
				"the first poll sends the ClientHello to the boot's host"))
		return {};
	ServerHello server_hello = build_server_hello(hello, kLocal.ip, kLocal.port);
	server_hello.hk = 0x55667788u;
	socket.inbox.push_back(
			nw_encode_outbound(SESSION_OPCODE_SERVER_HELLO, server_hello_to_bytes(server_hello)));
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (std::chrono::steady_clock::now() < deadline) {
		const std::size_t before = socket.sent.size();
		role.poll_preload();
		for (std::size_t i = before; i < socket.sent.size(); ++i) {
			ClientAuth auth;
			if (!opcode_of(socket.sent[i], opcode, body) || opcode != SESSION_OPCODE_CLIENT_AUTH ||
					!parse_client_auth(body.data(), body.size(), auth))
				continue;
			for (const std::vector<uint8_t> &chunk : auth.cu) {
				uint8_t type = 0;
				std::string name;
				std::string value;
				if (parse_client_cu_chunk(chunk.data(), chunk.size(), type, name, value) &&
						name == "APPID")
					return value;
			}
			return {};
		}
	}
	check(false, "the ClientAuth left within two seconds of the ServerHello");
	return {};
}

// A charattr.def table that read: the Medic class's ATTRIBUTES word.
inmatch::CharAttrTable loaded_charattr() {
	inmatch::CharAttrTable table;
	const std::string def = "[CHARACTER2]\r\nATTRIBUTES\t= Medic\r\n";
	inmatch::charattr_load(table, reinterpret_cast<const uint8_t *>(def.data()), def.size());
	return table;
}

struct Boot {
	std::unique_ptr<mission::MissionKernel> kernel = std::make_unique<mission::MissionKernel>();
	inmatch::JoinerRole role;
	ScriptedSocket socket;
	inmatch::CharAttrTable charattr = loaded_charattr();
	inmatch::JoinerBootRequest request;

	Boot() {
		kernel->world.registry.configure_pool(0, 16);
		role.bind(*kernel);
		request.protocol = title_protocol("jo");
		request.rules = &world::title_rules("jo");
		request.socket = &socket;
		request.host = kHost;
		request.local_address = kLocal;
		request.player_name = "BootJoiner";
		inmatch::CharacterJoinVars vars =
				inmatch::character_join_vars(inmatch::JoinCharacterProfile{});
		vars.char_id[0] = 0x2101;
		request.settings.character_vars = vars;
		request.settings.expansion_version_root = "boot-root";
		request.settings.app_id = "3225";
		request.settings.cd_cookie = { 'P', 'U', 'B', '1', 0 };
		request.settings.glsvss_request = "GLSVSS";
		request.settings.sync_tail_loading = true;
		request.settings.mission_data_loading = true;
		request.tables.charattr = &charattr;
		request.tables.auto_medic_off = 1u;
	}
};

void runtime_took(const inmatch::ClientRuntime &runtime, const mission::MissionKernel &kernel,
		const char *which) {
	const std::string head = std::string(which) + ": ";
	check(runtime.charattr_table() != nullptr && runtime.charattr_table()->loaded,
			(head + "the charattr.def table installed").c_str());
	check(kernel.world.tables.class_cammo_installed,
			(head + "the World took the table's per-class words").c_str());
	check(runtime.character_join_vars() != nullptr &&
					runtime.character_join_vars()->char_id[0] == 0x2101,
			(head + "the side profile installed").c_str());
	check(runtime.expansion_version_root() == "boot-root",
			(head + "the JOIN checksum's root installed").c_str());
}

void boot_dials_with_every_setting() {
	Boot boot;
	inmatch::ClientRuntime &runtime = inmatch::boot_joiner(boot.role, boot.request);
	check(boot.role.runtime.get() == &runtime, "the boot's runtime is the role's");
	runtime_took(runtime, *boot.kernel, "boot");
	check(!boot.role.started(), "the boot sends nothing: the first poll's hello is the role's");
	check(client_auth_app_id(boot.role, boot.socket) == "3225",
			"boot: the ClientAuth carries the settings' APPID");
}

void rebuilt_runtime_takes_the_same() {
	Boot boot;
	(void)inmatch::boot_joiner(boot.role, boot.request);
	// A load's bring-up rebuilds the runtime no hello went out from; the embedder's tail installs
	// the join's settings on it (Simulation's role_bringup_tail).
	check(boot.role.bring_up(), "a load rebuilds the runtime no hello went out from");
	check(boot.role.runtime != nullptr, "the rebuild leaves a runtime");
	if (!boot.role.runtime)
		return;
	inmatch::install_joiner_runtime(boot.role, boot.request.settings, boot.request.tables);
	runtime_took(*boot.role.runtime, *boot.kernel, "rebuild");
	check(client_auth_app_id(boot.role, boot.socket) == "3225",
			"rebuild: the ClientAuth carries the settings' APPID");
}

void streamed_types_settle_in_order() {
	const std::size_t stride = static_cast<std::size_t>(world::PF_STRIDE);
	std::vector<float> rows(stride * 5, 0.0f);
	const float types[] = { 5.0f, 0.0f, 7.0f, 5.0f, 9.0f };
	for (std::size_t i = 0; i < 5; ++i)
		rows[i * stride + world::PF_TYPE_ID] = types[i];
	const std::vector<int32_t> order = inmatch::streamed_type_order(rows);
	check(order == std::vector<int32_t>({ 5, 7, 9 }),
			"the stream's types in first-seen order, 0 skipped");

	Boot boot;
	(void)inmatch::boot_joiner(boot.role, boot.request);
	std::vector<std::string> trace;
	std::vector<int32_t> seated;
	inmatch::StreamedTypeLegs legs;
	legs.seat_specs = [&](const std::vector<int32_t> &type_ids) {
		trace.push_back("seat_specs");
		seated = type_ids;
	};
	legs.item_traits = [&] { trace.push_back("item_traits"); };
	legs.collision = [&] { trace.push_back("collision"); };
	inmatch::settle_streamed_types(boot.role, *boot.kernel, order, legs);
	check(trace == std::vector<std::string>({ "seat_specs", "item_traits", "collision" }),
			"the seat table, the traits, then the collision");
	check(seated == order, "the seat table over the stream's types");
}

} // namespace

int main() {
	boot_dials_with_every_setting();
	rebuilt_runtime_takes_the_same();
	streamed_types_settle_in_order();
	if (failures == 0)
		std::printf("joiner_boot: all passed\n");
	return failures == 0 ? 0 : 1;
}
