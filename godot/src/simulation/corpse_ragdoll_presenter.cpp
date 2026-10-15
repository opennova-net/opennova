#include "simulation/corpse_ragdoll_presenter.h"

#include "object/object_model.h"
#include "object/skeletal_anim.h"
#include "simulation/entity_presenter.h"
#include "simulation/simulation.h"

#include <base/io/fixed.h>
#include <base/io/tick_rate.h>
#include <runtime/anim/skeletal_clips.h>
#include <runtime/anim/skeletal_pose.h>
#include <runtime/ragdoll/ragdoll_rig.h>
#include <runtime/world/ai.h>
#include <runtime/world/corpse_ragdoll.h>
#include <runtime/world/world.h>

#include <limits>

namespace godot {

namespace {

using opennova::ragdoll::Xform;

// The local avatar's key: no wire handle reaches bit 31.
constexpr uint32_t kAvatarKey = 0x80000000u;
constexpr double kTickSeconds = 1.0 / opennova::io::kTickHz;

Xform xform_of(const ObjectModel *p_model) {
	const Transform3D t = p_model->get_global_transform();
	const Quaternion q = t.basis.get_rotation_quaternion();
	Xform x;
	x.rotation = opennova::anim::Quat{ static_cast<float>(q.w), static_cast<float>(q.x),
		static_cast<float>(q.y), static_cast<float>(q.z) };
	x.origin = opennova::anim::Vec3{ static_cast<float>(t.origin.x), static_cast<float>(t.origin.y),
		static_cast<float>(t.origin.z) };
	x.scale = static_cast<float>(t.basis.get_scale().x);
	return x;
}

// A seed every machine derives alike for the same corpse.
uint32_t kick_seed(uint32_t p_key, int32_t p_life) {
	return p_key * 0x9E3779B1u ^ static_cast<uint32_t>(p_life) * 0x85EBCA77u;
}

} // namespace

void CorpseRagdollPresenter::set_enabled(bool p_enabled) {
	if (p_enabled == set_.enabled()) {
		return;
	}
	if (!p_enabled) {
		clear();
	}
	set_.set_enabled(p_enabled);
}

void CorpseRagdollPresenter::clear() {
	for (const auto &[key, id] : drawn_) {
		if (ObjectModel *model = Object::cast_to<ObjectModel>(ObjectDB::get_instance(id))) {
			model->clear_body_pose_override();
		}
	}
	drawn_.clear();
	seen_.clear();
	rigs_.clear();
	set_.clear();
}

std::shared_ptr<const opennova::ragdoll::Rig> CorpseRagdollPresenter::rig_for(
		ObjectModel *p_model) {
	const Ref<SkeletalAnim> anim = p_model->get_skeletal_anim();
	if (anim.is_null() || !anim->is_loaded()) {
		return nullptr;
	}
	const opennova::anim::SkeletalClips &clips = anim->native_rig();
	auto it = rigs_.find(&clips);
	if (it != rigs_.end()) {
		return it->second.rig;
	}
	opennova::ragdoll::RigSource source;
	source.parents = clips.parents();
	for (const opennova::anim::ClipBone &bone : clips.bones()) {
		source.bn.push_back(opennova::anim::model_bone_index_from_name(bone.name));
	}
	clips.eval_pose(std::string(), 0.0, 0, source.rest);
	auto rig = std::make_shared<const opennova::ragdoll::Rig>(opennova::ragdoll::build_rig(source));
	RigEntry entry;
	entry.anim = anim;
	entry.rig = rig->valid() ? rig : nullptr;
	rigs_.emplace(&clips, entry);
	return entry.rig;
}

void CorpseRagdollPresenter::observe(ObjectModel *p_model, uint32_t p_key, int32_t p_life,
		int32_t p_anim_state, bool p_hidden, bool p_eligible,
		const opennova::ragdoll::Collider &p_collider, float p_water_y) {
	opennova::ragdoll::SubjectHead head;
	head.key = p_key;
	head.life = p_life;
	const opennova::world::CorpseDeath death =
			opennova::world::corpse_death(p_anim_state, kick_seed(p_key, p_life));
	head.dying = death.dying;
	head.ragdolls = death.ragdolls;
	head.kick = death.kick;
	head.eligible = p_eligible;
	head.hidden = p_hidden;
	head.node = xform_of(p_model);
	seen_[p_key] = p_model->get_instance_id();

	const opennova::ragdoll::Need need = set_.observe(head);
	if (need == opennova::ragdoll::Need::None) {
		return;
	}
	std::vector<opennova::anim::PoseBone> pose;
	if (!p_model->eval_body_clip_pose(0.0, pose) || pose.empty()) {
		return;
	}
	// The simulation's corpse: its hips, the rig's root row, under the model.
	const opennova::anim::Vec3 anchor = opennova::ragdoll::xform_point(head.node, pose[0].origin);
	if (need == opennova::ragdoll::Need::Run) {
		set_.feed(p_key, head.node, anchor);
		return;
	}
	// A body that dies in the water keeps retail's float.
	if (anchor.y <= p_water_y) {
		return;
	}
	const std::shared_ptr<const opennova::ragdoll::Rig> rig = rig_for(p_model);
	if (rig == nullptr) {
		return;
	}
	std::vector<opennova::anim::PoseBone> prev;
	p_model->eval_body_clip_pose(-kTickSeconds, prev);
	set_.start(head, rig, pose, prev.empty() ? nullptr : &prev, anchor, p_collider);
}

void CorpseRagdollPresenter::present(
		EntityPresenter *p_entities, Simulation *p_sim, ObjectModel *p_avatar, int p_ticks) {
	if (p_entities != nullptr) {
		p_entities->set_collect_ragdoll_rows(set_.enabled());
	}
	if (!set_.enabled() || p_sim == nullptr || p_sim->kernel_ == nullptr) {
		return;
	}
	const opennova::world::World &world = p_sim->kernel_->world;
	const opennova::world::WorldRagdollCollider collider(world);
	const float water_y = world.env.water_z != 0
			? static_cast<float>(world.env.water_z) * opennova::io::kInvFp16One
			: -std::numeric_limits<float>::infinity();

	seen_.clear();
	if (p_entities != nullptr) {
		for (const EntityPresenter::RagdollRow &row : p_entities->take_ragdoll_rows()) {
			observe(row.model, static_cast<uint32_t>(row.handle) & 0xFFFFu, row.life,
					row.anim_state, row.hidden, !row.severed, collider, water_y);
		}
	}
	if (p_avatar != nullptr && p_avatar->get_skeletal_anim().is_valid() &&
			world.cached.local_player.valid()) {
		if (const opennova::world::AiEntity *self =
						world.ai.for_handle(world.cached.local_player)) {
			observe(p_avatar, kAvatarKey, 0, self->inf.body_clip_state(), !p_avatar->is_visible(),
					true, collider, water_y);
		}
	}

	set_.step(MIN(p_ticks, 8), collider);

	for (const auto &[key, id] : seen_) {
		ObjectModel *model = Object::cast_to<ObjectModel>(ObjectDB::get_instance(id));
		if (model == nullptr) {
			continue;
		}
		if (const opennova::ragdoll::RagdollOutput *out = set_.output(key)) {
			model->set_body_pose_override(out->pose, out->weight);
			drawn_[key] = id;
		} else if (drawn_.erase(key) != 0) {
			model->clear_body_pose_override();
		}
	}
	// A drawn key whose model went away, or that the set dropped unseen.
	for (auto it = drawn_.begin(); it != drawn_.end();) {
		ObjectModel *model = Object::cast_to<ObjectModel>(ObjectDB::get_instance(it->second));
		if (model == nullptr) {
			it = drawn_.erase(it);
		} else if (set_.output(it->first) == nullptr) {
			model->clear_body_pose_override();
			it = drawn_.erase(it);
		} else {
			++it;
		}
	}
}

} // namespace godot
