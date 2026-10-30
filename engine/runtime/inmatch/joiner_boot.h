#pragma once

// The joiner's bring-up every joiner embedder runs, the joiner's half of host_boot.h: the game's
// Simulation (a menu join, a NovaWorld join, a direct mission load's) and a headless embedder (the
// two-peer ctests, a headless joiner) call the same steps, so the composition of a join exists
// once. Before it the order lived in the Godot binding (Simulation::enable_join and its fourteen
// install seams, the role's bring-up tail, and the wire-header join's model-derived legs inside the
// local player's visuals), and a load's rebuild of a not-yet-started runtime took only part of what
// the dial's took (its app id, CD cookie, GLSVSS request, browse row and connect log were lost).
//
//   boot_joiner: the role's protocol, rules, process words and server logs, its socket and its
//     peer, the join socket's own address, the runtime (JoinerRole::create_runtime: the callsign,
//     the join role and the passwords), the join's network type, the enumerator drain, every
//     runtime's settings and tables (install_joiner_runtime), and the role's join reset. The
//     embedder opens the socket ahead of it (a browse row's transport, or a dial over the client
//     port range: the platform's) and enters the session's connect past it.
//   install_joiner_runtime: what every runtime the join builds takes, the dial's and a load's
//     rebuild alike (JoinerRole::bring_up rebuilds one when no hello went out).
//   settle_streamed_types: a header-only join's model-derived legs once its stream's types are in,
//     ahead of the C2S 0x3D page's freeze (mission/mission_load.h runs it there).
//
// The join's network words are retail's client half of CNapiNetwork_StartClientConnection
// @0x4CA160 and its JOIN; the per-step witnesses sit on the runtime's setters
// (inmatch/joiner_connection.h) and in joiner_boot.cpp.

#include <net/npwire/idatagram_socket.h>
#include <net/npwire/peer_addr.h>
#include <runtime/inmatch/charattr_table.h>
#include <runtime/inmatch/character_registry.h>
#include <runtime/inmatch/join_role.h>
#include <runtime/inmatch/joiner_connection.h>
#include <runtime/inmatch/napi_np_connection.h> // CharacterJoinVars
#include <runtime/inmatch/napi_np_server_ctx.h> // NetworkType
#include <runtime/inmatch/server_files.h> // ServerLogs

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace opennova {
struct Protocol;
} // namespace opennova

namespace opennova::world {
struct TitleRules;
} // namespace opennova::world

namespace opennova::mission {
class MissionKernel;
} // namespace opennova::mission

namespace opennova::replication {
class ItemReplicationCatalog;
} // namespace opennova::replication

namespace opennova::inmatch {

class ClientRuntime;
class ConnectLog;
class JoinerRole;
struct ClientProcessState;

// The join's own words, which the embedder holds for the join's whole run and every runtime it
// builds takes (install_joiner_runtime).
struct JoinerSettings {
	// The profile's packed Avatars.def selection for each side, the JOIN's (unset: the
	// runtime's own).
	std::optional<CharacterJoinVars> character_vars;
	// The install root the JOIN's VERSIONCRCSTRING reads loose expansion/<name>/version.txt
	// under (D-NET-166); empty keeps the golden "0".
	std::string expansion_version_root;
	// The game-session APPID join token a NovaWorld host validates (reject code 9); "0" on a LAN.
	std::string app_id = "0";
	// The CD identity cookie for the JOIN (codes 23/24/25), empty on a LAN, and the gate's
	// GLSVSSREQUEST for a NovaWorld JOIN (D-NET-723).
	std::vector<uint8_t> cd_cookie;
	std::string glsvss_request;
	// The browse row's 0x81 record a join from it connects with (no 0x41).
	JoinerConnection::DiscoveredSession discovered_session;
	// The embedder's loads at the S2C 0x11 (the terrain, ahead of the 0x0A) and at the final S2C
	// 0x64 (the effect system, ahead of the 0x09).
	bool sync_tail_loading = false;
	bool mission_data_loading = false;
	// The join's network type: a NovaWorld join's session destroy keeps its transport, and its
	// loading steps read the socket (ClientRuntime::tear_down_transport, D-NET-1209).
	NetworkType network_type = NetworkType::Lan;
};

// The process's tables every runtime of the join reads (null: none).
struct JoinerTables {
	// charattr.def as the process loaded it before joining (Game_Run's boot read), whose
	// per-class words the World takes with it (install_class_attribute_flags).
	const CharAttrTable *charattr = nullptr;
	// The items.def replication catalog the decode view resolves wire classes through.
	std::shared_ptr<const replication::ItemReplicationCatalog> item_catalog;
	// The mounted Avatars.def's character table (null: none mounted).
	const CharacterRegistry *character_registry = nullptr;
	// The profile record's auto-medic word, raw (unset: no profile).
	std::optional<uint32_t> auto_medic_off;
	// The process's _connectlog.txt (null: off).
	ConnectLog *connect_log = nullptr;
};

struct JoinerBootRequest {
	// The title the join speaks and the rules its World and replica run.
	const Protocol *protocol = nullptr;
	const world::TitleRules *rules = nullptr;
	// The process's client words (null: the role's own) and its server logs (null: off).
	ClientProcessState *process = nullptr;
	const ServerLogs *server_logs = nullptr;
	// The socket the embedder opened, the host it reaches, and the address it bound.
	IDatagramSocket *socket = nullptr;
	PeerAddr host{};
	PeerAddr local_address{};
	// The join's identity: the callsign and the three passwords in the profile's code-page bytes
	// (D-NET-661, D-HUD-57), and the join role.
	std::string player_name;
	JoinRole join_role = JoinRole::Player;
	std::string spectator_password;
	std::string server_password;
	std::string join_password;
	// The session reset's connection destroy drains the process enumerator's records (the browse
	// the join went on from, D-NET-956). Empty: none.
	std::function<void()> enumerator_drain;
	JoinerSettings settings;
	JoinerTables tables;
};

// The join's bring-up over `role` (installed and bound by the embedder), in the order above. The
// runtime it built (role.runtime).
ClientRuntime &boot_joiner(JoinerRole &role, const JoinerBootRequest &request);

// Every runtime's settings and tables onto `role`'s runtime; a no-op with none.
void install_joiner_runtime(
		JoinerRole &role, const JoinerSettings &settings, const JoinerTables &tables);

// A header-only join's model-derived legs, the embedder's over its own retained sources: the seat
// table for the stream's types in first-seen order, the item traits sweep, and the collision
// instances. The embedder wires its asset source first (the seat extraction reads model
// userpoints through it).
struct StreamedTypeLegs {
	std::function<void(const std::vector<int32_t> &type_ids)> seat_specs;
	std::function<void()> item_traits;
	std::function<void()> collision;
};

// The streamed types' unique runtime type ids in present-row order (world/present_rows.h, `rows`
// PF_STRIDE floats each), 0 skipped: the order the load's model warm resolves them in.
std::vector<int32_t> streamed_type_order(const std::vector<float> &rows);

// The header-only join learned its entity types from the stream past its mission body's setup:
// the model-derived seat and emplacement table and the trait and collision consumers resolve now,
// ahead of the admission and the C2S 0x3D page's freeze, and every streamed row takes its class
// init's slot leg once the table and the traits are in (JoinerRole::
// run_streamed_weapon_slot_class_inits), then the mission-start portal init
// (MissionKernel::occlusion_init_mission). Retail's mission start initializes every pool's rows
// from their models after the def reload.
// [orig: Game_StartMission @0x524360 - Game_ReloadEntityModelsAndCallbacks called @0x52550E,
//  Entity_InitAllFromModels called @0x52567F]
void settle_streamed_types(JoinerRole &role, mission::MissionKernel &kernel,
		const std::vector<int32_t> &type_order, const StreamedTypeLegs &legs);

} // namespace opennova::inmatch
