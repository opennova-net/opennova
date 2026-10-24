// A joiner's world composes an addeweap child at its carrier's articulated
// anchor from the gun words a REMOTE UseGun rider drives (D-NET-690's ewep
// update on a joiner). Retail's joiner holds the remote gunner as an entity
// attached to the carrier's UseGun seat, and that seat's call publishes the
// carrier's gun words to the CTRL bus whatever the carrier's render class, so
// the child's Bone_BuildAttachmentMatrix poses it on the live EWEAP frame. Ours
// keeps a remote rider replica-only, so the world must learn the rider from the
// replica row (the same frame sequence coop_two_sim's attachment test drives
// through Godot: a carrier with no ewep render class, an ewep child anchored on
// a userpoint the EWEAP_GUNYAW part carries, a remote gunner on its Usegun seat).
// [orig: Entity_AttachToBoneAndUpdateTransform @0x546517..0x546518 (the seat
//  call's ewep writer); Entity_UpdateTransformAndTurret @0x440CA0 ->
//  Bone_BuildAttachmentMatrix @0x44109D]

#include <base/resource_index/resource_index.h>
#include <base/gameprofile/gameprofile.h>
#include <formats/def/def.h>
#include <formats/threedi/threedi_ctrl_catalog.h>
#include <net/title/title_identity.h>
#include <net/npwire/entity_class.h>
#include <net/npwire/idatagram_socket.h>
#include <net/npwire/peer_addr.h>
#include <runtime/assets/asset_store.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/join_role.h>
#include <runtime/inmatch/joiner_role.h>
#include <runtime/inmatch/session.h>
#include <runtime/mission/mission_kernel.h>
#include <runtime/mission/promote.h> // mission::ItemSeatSpec
#include <runtime/mission/seat_spec_extract.h>
#include <runtime/replication/client_state.h>
#include <runtime/world/angle.h>
#include <runtime/world/entity.h>
#include <runtime/world/mounted_pose.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/world.h>

#include "common/file_io.h"
#include "common/test_paths.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

static const opennova::TitleIdentity &kJo = *opennova::title_identity(opennova::gameprofile::GAME_JO);

namespace {

using namespace opennova;
namespace inmatch = opennova::inmatch;
namespace w = opennova::world;
using namespace opennova::threedi;

int failures = 0;
bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	++failures;
	return false;
}

constexpr uint32_t kClientKey = 0x0000BEEFu;
constexpr uint32_t kSessionId = 0x0FE0E112u;
constexpr uint16_t kSelfHandle = 0x0005;
constexpr uint16_t kRemoteGunner = 0x0006;
constexpr int32_t kCarrierType = 5004;
constexpr int32_t kChildType = 1419;

class SilentSocket final : public opennova::IDatagramSocket {
public:
	int recv_from(uint8_t *, std::size_t, PeerAddr &) override { return 0; }
	void send_to(const PeerAddr &, const uint8_t *, std::size_t) override {}
};

// coop_two_sim's authored carrier row (no render class of its own, its child
// anchored at `anchor`) and the B50's ewep row; the sibling table repeats the
// child type on a second, missing-anchor row (stored slot 2).
std::string items_text(const std::string &anchor, bool sibling) {
	return "begin \"Attachment Fixture Carrier\"\r\n id 105004\r\n graphic mount\r\n"
	       " primary_weapon WPN_EMPLCD50NA\r\n addeweap " + anchor + " 101419\r\n" +
	       (sibling ? " addeweap missing 101419\r\n" : "") + "end\r\n"
	       "begin \"NON-Armored Emplaced 50cal for FAV\"\r\n id 101419\r\n type object\r\n"
	       " graphic mount\r\n attrib: EWeap forceasset\r\n HP 10\r\n ai_function ewep\r\n"
	       " move_function ewep\r\n render_function ewep\r\n primary_weapon WPN_EMPLCD50NA\r\nend\r\n";
}

bool extract(const std::string &anchor, const assets::AssetStore &store,
		mission::SeatSpecExtraction &out, bool sibling = false) {
	const std::string text = items_text(anchor, sibling);
	def::DefItemsFile items{};
	if (def::def_parse_items_memory(reinterpret_cast<const uint8_t *>(text.data()), text.size(),
			    &items) != 0)
		return false;
	out = {};
	mission::extract_item_seat_specs(items,
			[&store](const std::string &graphic) { return store.model(graphic).get(); },
			{105004}, out);
	def::def_free_items(&items);
	return true;
}

const mission::ItemSeatSpec *spec_for(const mission::SeatSpecExtraction &extraction, int32_t type) {
	for (const mission::ItemSeatSpec &spec : extraction.specs)
		if (spec.type_id == type) return &spec;
	return nullptr;
}

float distance(const w::Vec3 &a, const w::Vec3 &b) {
	return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) +
			(a.z - b.z) * (a.z - b.z));
}

// The userpoint the EWEAP_GUNYAW part carries (coop_two_sim's
// _moving_eweap_userpoint): its name, resolved by the native extractor.
std::string moving_eweap_anchor(const threedi::Threedi3di3 &model, const assets::AssetStore &store) {
	w::Entity carrier;
	carrier.item_id = kCarrierType;
	for (size_t u = 0; u < model.user_point_count; ++u) {
		const std::string name = model.user_points[u].name;
		if (name.empty() || name.find(' ') != std::string::npos) continue;
		mission::SeatSpecExtraction extraction;
		if (!extract(name, store, extraction)) continue;
		const mission::ItemSeatSpec *spec = spec_for(extraction, kCarrierType);
		if (spec == nullptr || spec->emplacement_attachments.empty()) continue;
		const mission::ItemEmplacementAttachmentSpec &attachment = spec->emplacement_attachments[0];
		if (!attachment.anchor_found || attachment.anchor.bone_index == 0) continue;
		int32_t neutral[THREEDI_CTRL_REGISTER_COUNT] = {};
		int32_t turned[THREEDI_CTRL_REGISTER_COUNT] = {};
		turned[THREEDI_CTRL_EWEAP_GUNYAW] = 16384;
		w::MountedPose rest, live;
		if (!w::resolve_model_mounted_pose(model, carrier, attachment.anchor, neutral, 0, rest) ||
				!w::resolve_model_mounted_pose(model, carrier, attachment.anchor, turned, 0, live))
			continue;
		if (distance(rest.position, live.position) > 0.25f) return name;
	}
	return {};
}

// The joiner frame over a kernel on the heap (a World-carrying frame is megabytes).
struct Harness {
	std::unique_ptr<mission::MissionKernel> kernel = std::make_unique<mission::MissionKernel>();
	inmatch::JoinerRole role;
	SilentSocket socket;
	inmatch::TickInput input;

	explicit Harness(const assets::AssetStore &store) {
		kernel->world.registry.configure_pool(0, 16);
		kernel->world.registry.configure_pool(1, 16);
		kernel->set_assets(&store);
		kernel->wire_collision(); // the kernel is the world's mounted-pose provider
		role.bind(*kernel);
		kernel->world.load_systems();
		role.set_socket(&socket, PeerAddr{});
		role.set_title(&kJo);
		role.create_runtime("UsegunJoiner", inmatch::JoinRole::Player, "", "");
		role.kit_seams.apply_authoritative = [] {};
		role.kit_seams.reseed_on_side_change = [] { return false; };
		role.kit_seams.push = [] {};
		role.poll_preload();
		role.runtime->seed_session(kSessionId, kClientKey, "CLIENT-BRIDGE-SCRK",
				"SERVER-BRIDGE-SCRK", 1, 0, kSelfHandle, w::kPlayerInfantryTypeId);
		role.run_tick(input);
		role.runtime->view().set_item_class_resolver(
				[](uint16_t type) -> std::optional<EntityClass> {
					if (type == kCarrierType || type == kChildType)
						return EntityClass::NoNetworkCallback;
					if (type == w::kPlayerInfantryTypeId) return EntityClass::Player;
					return std::nullopt;
				});
	}
};

// D-NET-690 in the Godot world's sequence: a remote gunner turns the carrier's
// gun, and the joiner's world moves the ewep child off its spawn pose with the
// articulated anchor, the decoded row reading the world's pose back.
bool run_remote_usegun_rider_drives_the_child_compose() {
	const std::string synth =
			std::string(test_paths_repo_root(__FILE__)) + "/fixtures/threedi/synth";
	if (test_io::is_lfs_pointer(test_io::read_file(synth + "/mount.3di"))) {
		std::printf("[skip] fixtures/threedi/synth/mount.3di is an unpulled LFS pointer\n");
		return true;
	}
	ResourceIndex index;
	if (!expect(index.scan(synth, std::string(), VfsMountMode::LooseOnly),
			    "the synthetic model directory indexes"))
		return false;
	assets::AssetStore store{&index};
	const assets::Model model = store.model("mount");
	if (!expect(model != nullptr, "mount.3di loads")) return false;
	const std::string anchor = moving_eweap_anchor(*model, store);
	if (!expect(!anchor.empty(), "mount exposes a userpoint the EWEAP_GUNYAW part carries"))
		return false;

	Harness h(store);
	bool ok = true;
	w::World &world = h.kernel->world;
	mission::SeatSpecExtraction extraction;
	ok &= expect(extract(anchor, store, extraction), "the authored rows parse");
	h.kernel->seat_specs = extraction.specs;
	h.kernel->mounted_graphics = extraction.graphic_by_type;
	const mission::ItemSeatSpec *carrier_spec = spec_for(extraction, kCarrierType);
	uint8_t gunner_bone = 0;
	if (carrier_spec != nullptr)
		for (const w::Seat &seat : carrier_spec->seats)
			if (seat.type == w::SeatType::Gunner && seat.bone_index != 0 && gunner_bone == 0)
				gunner_bone = seat.bone_index;
	if (!expect(gunner_bone != 0, "the mount graphic gives the carrier a Usegun seat"))
		return false;

	// The carrier and its addeweap child, as the joiner's world holds them.
	w::Entity carrier_seed;
	carrier_seed.kind = w::EntityKind::Item;
	carrier_seed.item_id = kCarrierType;
	carrier_seed.spawn_origin = (1u << 24) | 3u;
	carrier_seed.position.set_q16({4 << 16, 6 << 16, 0});
	const w::EntityHandle carrier = world.registry.spawn(1, carrier_seed);
	mission::refresh_item_seat_spec(world, h.kernel->seat_specs, *world.registry.get(carrier), false);
	w::Entity child_seed;
	child_seed.kind = w::EntityKind::Item;
	child_seed.item_id = kChildType;
	child_seed.spawn_origin = (1u << 24) | 4u;
	child_seed.position = world.registry.get(carrier)->position;
	child_seed.emplaced_update = true;
	child_seed.emplacement_parent = carrier;
	child_seed.emplacement_parent_spawn_id = world.registry.get(carrier)->registry_spawn_id;
	const w::EntityHandle child = world.registry.spawn(1, child_seed);
	w::Entity &child_row = *world.registry.get(child);
	mission::refresh_item_seat_spec(world, h.kernel->seat_specs, child_row, false);
	ok &= expect(child_row.emplacement_pose_metadata_resolved && child_row.emplacement_bone != 0,
			"the child resolves its articulated anchor on the carrier");

	// Their decoded rows and the remote gunner on the carrier's Usegun seat.
	replication::ClientState &state = h.role.runtime->state();
	replication::ClientEntityState &carrier_es = state.upsert(carrier.packed);
	carrier_es.type_id = static_cast<uint16_t>(kCarrierType);
	carrier_es.cls = EntityClass::NoNetworkCallback;
	carrier_es.x = 4 << 16;
	carrier_es.y = 6 << 16;
	replication::ClientEntityState &child_es = state.upsert(child.packed);
	child_es.type_id = static_cast<uint16_t>(kChildType);
	child_es.cls = EntityClass::NoNetworkCallback;
	child_es.x = carrier_es.x;
	child_es.y = carrier_es.y;
	child_es.target_handle = carrier.packed;
	child_es.spawn_revision = 1;
	replication::ClientEntityState &gunner = state.upsert(kRemoteGunner);
	gunner.type_id = static_cast<uint16_t>(w::kPlayerInfantryTypeId);
	gunner.cls = EntityClass::Player;
	gunner.carrier_handle = carrier.packed;
	gunner.mount_bone = gunner_bone;
	gunner.heading_bam = 0;
	for (int i = 0; i < 4; ++i) h.role.run_tick(h.input);
	const w::Vec3 before = world.registry.get(child)->position;

	// The remote gunner turns: the carrier's gun words follow the look, and the
	// child rides the turned anchor.
	state.find(kRemoteGunner)->heading_bam = w::bam_heading_from_mission_yaw_deg(30);
	for (int i = 0; i < 4; ++i) h.role.run_tick(h.input);
	const w::Entity *live = world.registry.get(child);
	const w::Entity *carrier_row = world.registry.get(carrier);
	ok &= expect(carrier_row != nullptr && carrier_row->emplaced_gun_yaw_word != 0,
			"the remote gunner's look drives the carrier's gun word");
	ok &= expect(live != nullptr && distance(live->position, before) > 0.05f,
			"the joiner's world moves the child with the articulated anchor");
	const replication::ClientEntityState *after = state.find(child.packed);
	ok &= expect(after != nullptr &&
					after->world_class_update ==
							replication::ClientEntityState::WorldClassUpdate::Ewep,
			"the child is the world's ewep update's");
	ok &= expect(after != nullptr && live != nullptr && after->x == live->position.x.q16 &&
					after->y == live->position.y.q16 && after->z == live->position.z.q16,
			"the decoded row reads the world's composed pose back");
	// The world's compose equals the carrier model posed on the carrier's words.
	const auto on_turned_anchor = [&]() {
		const w::Entity *posed = world.registry.get(child);
		const w::Entity *hull = world.registry.get(carrier);
		if (posed == nullptr || hull == nullptr) return false;
		int32_t ctrl[THREEDI_CTRL_REGISTER_COUNT] = {};
		ctrl[THREEDI_CTRL_EWEAP_GUNYAW] = static_cast<uint16_t>(hull->emplaced_gun_yaw_word);
		ctrl[THREEDI_CTRL_EWEAP_GUNPITCH] = static_cast<uint16_t>(hull->emplaced_gun_pitch_word);
		w::Seat seat;
		seat.type = w::SeatType::Gunner;
		seat.bone_index = posed->emplacement_bone;
		seat.seat_local = posed->emplacement_local;
		seat.yaw_offset = posed->emplacement_yaw_offset;
		seat.attachment_frame = true;
		w::MountedPose expected;
		return seat.bone_index != 0 &&
				w::resolve_model_mounted_pose(*model, *hull, seat, ctrl,
						w::mounted_pose_time_ms(world.logic_tick, -1), expected) &&
				distance(expected.position, posed->position) < 1.0e-3f &&
				distance(expected.position, before) > 0.05f;
	};
	ok &= expect(on_turned_anchor(), "the child sits on the anchor the carrier's gun words pose");

	// A reinstalled table with a same-type sibling row leaves the child on the
	// anchor its subType names (coop_two_sim's last step: the install refreshes
	// the live rows).
	mission::SeatSpecExtraction sibling;
	ok &= expect(extract(anchor, store, sibling, true), "the sibling table parses");
	h.kernel->seat_specs = sibling.specs;
	h.kernel->mounted_graphics = sibling.graphic_by_type;
	mission::refresh_item_seat_spec(world, h.kernel->seat_specs, *world.registry.get(carrier), false);
	mission::refresh_item_seat_spec(world, h.kernel->seat_specs, *world.registry.get(child), false);
	for (int i = 0; i < 5; ++i) h.role.run_tick(h.input);
	ok &= expect(on_turned_anchor(), "a same-type sibling row leaves the child on its subType's anchor");

	// A dead or hidden rider's body never reaches the seat call: its carrier
	// stops publishing. [orig: Entity_UpdateInfantryPlayerBody @0x4B411E;
	// Entity_AttachToBoneAndUpdateTransform @0x5463FC]
	for (const uint8_t bit : {static_cast<uint8_t>(w::kEntityFlagDead),
			     static_cast<uint8_t>(w::kEntityFlagCarried)}) {
		replication::ClientEntityState &rider = *state.find(kRemoteGunner);
		rider.state_flags = bit;
		rider.state_flags_known = true;
		h.role.run_tick(h.input);
		ok &= expect(!world.registry.get(carrier)->replica_usegun_rider,
				"a dead or hidden remote rider publishes nothing");
		rider.state_flags = 0;
		h.role.run_tick(h.input);
		ok &= expect(world.registry.get(carrier)->replica_usegun_rider,
				"a living remote rider publishes again");
	}
	return ok;
}

} // namespace

int main() {
	run_remote_usegun_rider_drives_the_child_compose();
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("joiner_usegun_rider_compose: ok\n");
	return 0;
}
