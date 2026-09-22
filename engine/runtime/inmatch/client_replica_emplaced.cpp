#include <runtime/inmatch/client_replica_emplaced.h>
#include <runtime/replication/client_state.h>
#include <runtime/mission/seat_spec_extract.h>
#include <runtime/world/mount_controls.h>

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
			spec.turret_yaw_range_bam, spec.turret_pitch_max_bam, spec.turret_pitch_min_bam);
	return window;
}
} //namespace

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
}
} //namespace opennova::inmatch
