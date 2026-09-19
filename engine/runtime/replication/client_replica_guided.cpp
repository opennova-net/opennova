// [orig: NetPacket_DispatchToEntityByNetId @0x4D6960 ->
// Entity_SerializeGuidedMissileState @0x447C50]. Guidance only mutates an
// existing round. The fire descriptor owns its birth, ammo and launch pose.
#include <runtime/replication/client_replica_pipeline.h>
#include <runtime/world/round_sim.h>
namespace opennova::replication {
void ClientReplicaPipeline::apply_entity_routed(const std::vector<uint8_t> &body) {
    EntityRoutedPacket packet;
    if (!decode_entity_routed_packet(body.data(), body.size(), packet)) { ++malformed_bodies_; return; }
    if (!guided_round_resolver_) return;
    world::LiveRound *round = guided_round_resolver_(packet.net_id);
    if (!round || !round->active || round->age_ticks >= round->max_age_ticks ||
        round->guided_family == world::GuidedFamily::None || (round->guided.flags & 1)) return;
    const auto group = static_cast<GuidedFieldGroup>(packet.subtype);
    GuidedRecord rec; size_t used = 0;
    if (!decode_guided_field_group(GuidedMode::ReadFull, group, packet.body, packet.body_size, rec, used)) return;
    auto &s = round->guided;
    if (rec.launched) { s.flags |= 1; round->det_at_expiry = true; }
    if (rec.target_cleared) { s.flags &= ~2u; s.target = 0xFFFF; s.timer = -1; }
    if (rec.target_bound) { s.flags |= 2; s.target = rec.target_slot; s.timer = -1; }
    if (group == GuidedFieldGroup::TargetTypePos) s.phase = rec.weapon_type;
    if (group == GuidedFieldGroup::TargetPos || group == GuidedFieldGroup::TargetTypePos || group == GuidedFieldGroup::Pos) {
        // Read-full stores every decoded coordinate, including an all-zero point.
        // [orig: Entity_SerializeGuidedMissileState @0x447C50, group 3
        //  @0x447EEB/@0x447F08/@0x447F2E, group 4 @0x4480E5/@0x448102/@0x448129,
        //  group 5 @0x448151/@0x44816E/@0x448195]
        s.steer[0] = rec.pos_x; s.steer[1] = rec.pos_y; s.steer[2] = rec.pos_z;
    }
    if (group == GuidedFieldGroup::AttachOffsets) {
        s.saved[0] = rec.attach_x; s.saved[1] = rec.attach_y; s.saved[2] = rec.attach_z;
    }
}
} // namespace opennova::replication
