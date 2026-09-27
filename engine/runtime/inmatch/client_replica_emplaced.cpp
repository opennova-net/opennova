#include <runtime/inmatch/client_replica_emplaced.h>
#include <runtime/replication/client_state.h>
#include <runtime/mission/seat_spec_extract.h>
#include <runtime/world/mount_controls.h>
#include <runtime/world/present_rows.h>
#include <runtime/world/vehicle_motor.h>

namespace opennova::inmatch {
namespace {
world::TurretWindow replica_window(const replication::ClientEntityState &mount,
		const replication::ClientState &state, const std::vector<mission::ItemSeatSpec> &specs,
		const mission::ItemSeatSpec &spec) {
	const mission::ItemEmplacementAttachmentSpec *arc = nullptr;
	if (mount.carrier_handle != world::EntityHandle::kInvalid) {
		for (const replication::ClientEntityState &carrier : state.entities) {
			if (carrier.handle != mount.carrier_handle)
				continue;
			if (const mission::ItemSeatSpec *carrier_spec =
							mission::item_seat_spec_for_type(specs, carrier.type_id)) {
				for (const mission::ItemEmplacementAttachmentSpec &candidate :
						carrier_spec->emplacement_attachments) {
					if (candidate.anchor_found && candidate.anchor.bone_index == mount.mount_bone &&
							(candidate.down_limit_bam | candidate.up_limit_bam |
									candidate.right_limit_bam | candidate.left_limit_bam) != 0) {
						arc = &candidate;
						break;
					}
				}
			}
			break;
		}
	}
	const world::TurretWindow window = world::select_turret_window_bam(
			arc != nullptr ? arc->down_limit_bam : 0, arc != nullptr ? arc->up_limit_bam : 0,
			arc != nullptr ? arc->right_limit_bam : 0, arc != nullptr ? arc->left_limit_bam : 0,
			spec.turret_limits_valid, spec.turret_yaw_range_bam, spec.turret_pitch_max_bam,
			spec.turret_pitch_min_bam);
	return window;
}

// The parent brain's profile type. A joiner row has no brain to read it
// from: its ai_function brain class stands in, else its motor family, as the
// rotor machine selects (vehicle_part_anim.h).
int32_t replica_parent_profile_type(const world::World &world, const world::Entity &parent) {
	if (const world::AiEntity *ai = world.ai.for_handle(parent.handle))
		return ai->profile.type;
	const world::VehicleTraits *traits = world.vehicles.traits.get(parent.item_id);
	if (traits == nullptr) return 0;
	if (traits->brain_class != world::VehicleBrainClass::Unset)
		return traits->brain_class == world::VehicleBrainClass::Air ? 1 : 2;
	return world::vehicle_family_uses_direct_air_mover(traits->family) ? 1 : 2;
}

// The ewep class update's parent publication, the joiner's form: through the
// same gates (an ewep class child with a slot Def, riding the unique authored
// attachment row of its carrier on an authored userpoint of the carrier's
// root subobject) the child's words land on the carrier's replica row. The
// carrier is the child's 0x0D TARGET (groundEntity), resolved by the one
// persistent-carrier rule; the 0x0D parent is the occupant back-reference.
// [orig: Entity_UpdateTransformAndTurret @0x440ca0 — Def gate
//  @0x440E8C..0x440EA0, userpoint gates @0x440f04..0x440f50, profile type
//  @0x440f65..0x440f6b, values @0x440f70..0x441020; groundEntity carrier read
//  @0x440CBF; NapiNPClientMsg_0x00D target store @0x4332D7]
void publish_replica_gun_words(replication::ClientState &state,
		const std::vector<mission::ItemSeatSpec> &specs, const world::World &world) {
	for (const replication::ClientEntityState &child : state.entities) {
		const uint16_t carrier_handle = replication::persistent_carrier_handle(child);
		if (carrier_handle == wire_handle::kInvalid) continue;
		const world::Entity *child_twin = world.registry.get(world::EntityHandle{child.handle});
		if (child_twin == nullptr || static_cast<uint16_t>(child_twin->item_id) != child.type_id ||
				!child_twin->emplaced_update || world::emplaced_slot_def(world, *child_twin) == nullptr)
			continue;
		replication::ClientEntityState *carrier = state.find(carrier_handle);
		if (carrier == nullptr) continue;
		const mission::ItemSeatSpec *carrier_spec =
				mission::item_seat_spec_for_type(specs, carrier->type_id);
		if (carrier_spec == nullptr) continue;
		const mission::ItemEmplacementAttachmentSpec *attachment = nullptr;
		bool ambiguous = false;
		for (const mission::ItemEmplacementAttachmentSpec &candidate :
				carrier_spec->emplacement_attachments) {
			if (candidate.child_type_id != static_cast<int32_t>(child.type_id)) continue;
			if (attachment != nullptr) ambiguous = true;
			attachment = &candidate;
		}
		if (ambiguous || attachment == nullptr || !attachment->anchor_found ||
				attachment->anchor.bone_index == 0 || attachment->anchor_subobject != 0)
			continue;
		const world::Entity *carrier_twin =
				world.registry.get(world::EntityHandle{carrier->handle});
		if (carrier_twin == nullptr ||
				static_cast<uint16_t>(carrier_twin->item_id) != carrier->type_id)
			continue;
		const world::EmplacedParentGunWords words = world::emplaced_parent_gun_words(world,
				*carrier_twin, replica_parent_profile_type(world, *carrier_twin),
				child.emplaced_gun_yaw_word, child.emplaced_gun_pitch_word);
		if (words.yaw) {
			carrier->carried_gun_yaw_word = world::emplaced_bam_word(words.yaw_bam);
			carrier->carried_gun_words_valid = true;
		}
		if (words.pitch)
			carrier->carried_gun_pitch_word = world::emplaced_bam_word(words.pitch_bam);
	}
}
} //namespace

// The tank render callback publishes VEHICLE_GUNYAW/GUNPITCH, and a GROUND
// brain (or the helicopter callback) the HELO pair, from the brain's active
// words; on a joiner the replica row carries the words its children
// published. [orig: HUD_CacheEntityDebugStats @0x449ECF..0x449EE2;
//  HUD_CacheEntityDisplayInfo @0x4A3D90]
void write_present_replica_vehicle_gun(float *record, const world::World &world,
		const world::Entity &twin, const replication::ClientEntityState &carrier) {
	if (!carrier.carried_gun_words_valid) return;
	const world::VehicleTraits *traits = world.vehicles.traits.get(twin.item_id);
	if (traits == nullptr || traits->render_family == world::VehicleRenderFamily::None) return;
	const int32_t profile_type = replica_parent_profile_type(world, twin);
	uint32_t mask = record[world::PF_VEHICLE_MOTION_VALID] == 1.0f
			? static_cast<uint32_t>(record[world::PF_VEHICLE_CTRL_MASK])
			: 0u;
	if (traits->render_family == world::VehicleRenderFamily::Tank || profile_type == 2)
		mask |= world::VC_VEHICLE_GUN;
	if (traits->render_family == world::VehicleRenderFamily::Helicopter ||
			(traits->render_family != world::VehicleRenderFamily::Tank && profile_type == 1))
		mask |= world::VC_HELO_GUN;
	record[world::PF_VEHICLE_MOTION_VALID] = 1.0f;
	record[world::PF_VEHICLE_CTRL_MASK] = static_cast<float>(mask);
	record[world::PF_VEHICLE_GUN_YAW] = static_cast<float>(carrier.carried_gun_yaw_word);
	record[world::PF_VEHICLE_GUN_PITCH] = static_cast<float>(carrier.carried_gun_pitch_word);
}

// Client emplacements run the same callbacks. Remote look belongs to
// ClientState; L's world body already ran the shared channel this tick.
// [orig: Entity_UpdateChildAttachment @0x4409A0;
//  Entity_UpdateTransformAndTurret @0x440CA0]
void tick_replica_emplaced_channels(replication::ClientState &state,
		const std::vector<mission::ItemSeatSpec> &specs, world::World &world,
		uint16_t self_handle) {
	for (auto &mount : state.entities) {
		// The materialized world row's phase, only while that row is still this
		// wire entity's type (a reused handle must not forward a foreign phase).
		const auto *spin_source = world.registry.get(world::EntityHandle{mount.handle});
		const bool same_type = spin_source != nullptr &&
				static_cast<uint16_t>(spin_source->item_id) == mount.type_id;
		mount.emplaced_spin_phase = same_type ? spin_source->emplaced_spin_phase : 0;
		// An 'ewep' render class publishes its held words whether or not a
		// gunner is seated: an emptied turret keeps its last traverse.
		// [orig: HUD_CacheWeaponSlotInfo @0x440930 via the 'ewep' render-class
		//  row @0x82CFA0, no occupant test]
		mount.emplaced_controls_valid = same_type && spin_source->emplaced_ctrl_publisher;
		const auto *spec = mission::item_seat_spec_for_type(specs, mount.type_id);
		if (spec == nullptr)
			continue;
		for (auto &occupant : state.entities) {
			if (occupant.carrier_handle != mount.handle || occupant.mount_bone == 0 ||
					(occupant.cls != EntityClass::Player && occupant.cls != EntityClass::Infantry))
				continue;
			bool gunner_seat = false;
			for (const auto &seat : spec->seats)
				if (seat.bone_index == occupant.mount_bone && seat.type == world::SeatType::Gunner)
					gunner_seat = true;
			if (!gunner_seat)
				continue;
			auto *local_mount = world.registry.get(world::EntityHandle{ mount.handle });
			const auto *local_gunner = world.ai.for_handle(world.cached.local_player);
			if (occupant.handle == self_handle && local_mount != nullptr &&
					local_gunner != nullptr &&
					local_mount->primary_weapon_owner == world.cached.local_player) {
				mount.emplaced_gun_yaw_word = local_mount->emplaced_gun_yaw_word;
				mount.emplaced_gun_pitch_word = local_mount->emplaced_gun_pitch_word;
				occupant.heading_bam = local_gunner->heading;
				occupant.pitch_bam = local_gunner->pitch;
			} else {
				world::EmplacedGunChannel channel{ mount.emplaced_gun_yaw_word,
					mount.emplaced_gun_pitch_word };
				world::EmplacedGunnerLook look{ occupant.heading_bam, occupant.pitch_bam,
					occupant.recoil_pitch, occupant.handle == self_handle,
					occupant.cls == EntityClass::Player };
				world::advance_emplaced_gun_channel(channel, look, mount.heading_bam,
						mount.pitch_bam,
						(spec->item_attrib2 & def::DEF_ITEM_ATTRIB2_ISTURRET) != 0);
				world::clamp_emplaced_gun_channel(channel, look, mount.heading_bam, mount.pitch_bam,
						replica_window(mount, state, specs, *spec));
				mount.emplaced_gun_yaw_word = channel.yaw;
				mount.emplaced_gun_pitch_word = channel.pitch;
				occupant.heading_bam = look.heading;
				occupant.pitch_bam = look.pitch;
				if (local_mount != nullptr) {
					local_mount->emplaced_gun_yaw_word = channel.yaw;
					local_mount->emplaced_gun_pitch_word = channel.pitch;
				}
			}
			occupant.yaw_byte =
					static_cast<uint8_t>(static_cast<uint32_t>(occupant.heading_bam) >> 24);
			occupant.pitch_byte =
					static_cast<uint8_t>(static_cast<uint32_t>(occupant.pitch_bam) >> 24);
			mount.emplaced_controls_valid = true;
			break;
		}
	}
	publish_replica_gun_words(state, specs, world);
}
} //namespace opennova::inmatch
