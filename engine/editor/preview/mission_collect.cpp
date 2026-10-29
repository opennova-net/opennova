#include <editor/preview/mission_collect.h>

#include <base/io/fixed.h>
#include <editor/preview/mission_world.h>
#include <formats/mission/bms.h>
#include <runtime/mission/mission_kernel.h>
#include <runtime/world/angle.h> // placed_heading_bam, spawn_angle_bam

namespace opennova::editor {

MissionCollect::MissionCollect() = default;
MissionCollect::~MissionCollect() = default;

void MissionCollect::load(const std::shared_ptr<MissionFiles> &files, const bms::File &mission,
		const MissionGround &ground, const std::vector<MissionEntityMark> &marks) {
	close();
	files_ = files;
	if (!files_) return;
	kernel_ = std::make_unique<mission::MissionKernel>();
	mission::MissionKernel &kernel = *kernel_;
	mission_world_load_into(*files_, mission, ground, [&](world::World &world) {
		// Each record's entity takes its place among the marks as its identity. An entity with no file identity
		// keeps none, as the game gates only a placed one (inmatch::collect_view_verdicts).
		std::vector<world::EntityHandle> placed;
		world.registry.for_each([&](const world::Entity &entity) {
			if (entity.bms_id != 0) placed.push_back(entity.handle);
		});
		for (const world::EntityHandle handle : placed) {
			world::Entity &entity = *world.registry.get(handle);
			const MissionEntityMark *mark = MissionWorld::mark_of(entity, marks);
			rows_.push_back(mark != nullptr ? mark->row : NodeId(0));
			entity.bms_id = int32_t(rows_.size());
			if (mark != nullptr) handles_[mark->row] = handle.packed;
		}
	}, kernel.world, kernel.collision, kernel.occlusion, kernel.collision_pose, collision_state_);
	// The mission's start over the loaded world, as the game's runs it ahead of its first frame.
	kernel.occlusion_init_mission();
}

bool MissionCollect::move(const MissionEntityMark &mark) {
	// The record's three 16.16 words and its angles in the spawn's forms (mission::promote_mission's seed).
	if (!stand(mark.row, mark.x, mark.y, mark.z)) return false;
	world::Entity *entity = kernel_->world.registry.get(world::EntityHandle{handles_.at(mark.row)});
	entity->heading = world::placed_heading_bam(mark.yaw);
	entity->pitch_bam = world::spawn_angle_bam(mark.pitch);
	entity->roll_bam = world::spawn_angle_bam(mark.roll);
	return true;
}

bool MissionCollect::stand(NodeId row, double x, double y, double z) {
	if (!kernel_) return false;
	const auto found = handles_.find(row);
	world::Entity *entity = found != handles_.end()
			? kernel_->world.registry.get(world::EntityHandle{found->second})
			: nullptr;
	if (entity == nullptr) return false;
	entity->position.set_q16({io::float_to_fp16_16_round_sat(x), io::float_to_fp16_16_round_sat(y),
			io::float_to_fp16_16_round_sat(z)});
	return true;
}

void MissionCollect::close() {
	kernel_.reset();
	files_.reset();
	collision_state_ = mission::CollisionResolveState();
	rows_.clear();
	handles_.clear();
	wire_latch_.clear();
}

void MissionCollect::collect(const world::OcclusionViewSpec &view, MissionCollectVerdicts &out) {
	out.dropped.clear();
	out.buildings.clear();
	if (!kernel_) return;
	mission::MissionKernel &kernel = *kernel_;
	world::OcclusionFrameCamera cam;
	world::occlusion_camera_from_view(view, cam);
	// The main scene's collect over the main view's frame words, as the game's
	// (Simulation::run_occlusion_frame's order: the frame, then the walk).
	kernel.occlusion.select_view(world::OcclusionView::kMain);
	kernel.occlusion.build_frame(kernel.world, kernel.collision, cam);
	inmatch::collect_view_verdicts(inmatch::PresentRowsContext{kernel}, wire_latch_, cam, culled_,
			culled_wire_, anchors_);
	inmatch::collect_building_verdicts(kernel.world, kernel.collision, kernel.occlusion, buildings_);
	for (const int32_t id : culled_)
		if (id > 0 && size_t(id) <= rows_.size() && rows_[size_t(id) - 1] != 0)
			out.dropped.push_back(rows_[size_t(id) - 1]);
	for (const inmatch::BuildingVerdict &verdict : buildings_)
		if (verdict.bms_id > 0 && size_t(verdict.bms_id) <= rows_.size() && rows_[size_t(verdict.bms_id) - 1] != 0)
			out.buildings.push_back({rows_[size_t(verdict.bms_id) - 1], verdict.packed, verdict.forced});
}

} // namespace opennova::editor
