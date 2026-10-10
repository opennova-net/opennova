#include <editor/preview/definition_effects.h>

#include <algorithm>

#include <base/io/strutil.h>
#include <base/io/tick_rate.h>
#include <editor/preview/effect_catalog.h>

namespace opennova::editor {

namespace {

bool same_vec(const particle::Vec3 &a, const particle::Vec3 &b) {
	return a.x == b.x && a.y == b.y && a.z == b.z;
}

bool same_pose(const particle::EffectPose &a, const particle::EffectPose &b) {
	return same_vec(a.position, b.position) && same_vec(a.right, b.right) && same_vec(a.up, b.up) &&
	       same_vec(a.forward, b.forward);
}

} // namespace

bool same_spawns(const std::vector<DefinitionSpawn> &a, const std::vector<DefinitionSpawn> &b) {
	if (a.size() != b.size()) return false;
	for (size_t i = 0; i < a.size(); ++i)
		if (a[i].effect != b[i].effect || a[i].tick != b[i].tick || a[i].source != b[i].source ||
		    a[i].point != b[i].point || !same_pose(a[i].pose, b[i].pose))
			return false;
	return true;
}

bool DefinitionEffects::plan(const PreviewEffectCatalog &catalog, uint64_t catalog_serial,
                             std::vector<DefinitionSpawn> spawns) {
	if (catalog_serial == catalog_serial_ && same_spawns(spawns, spawns_)) return false;
	// More spawns after the same ones (a weapon's shots as its range runs, DI-22), each of an effect the scene
	// holds: the scene plays on, the new ones made as the clock reaches them, or at once pre-aged by their age.
	if (catalog_serial == catalog_serial_ && scene_ && spawns.size() > spawns_.size()) {
		const std::vector<DefinitionSpawn> head(spawns.begin(), spawns.begin() + std::ptrdiff_t(spawns_.size()));
		bool known = same_spawns(head, spawns_);
		for (size_t i = spawns_.size(); known && i < spawns.size(); ++i) {
			bool held = false;
			for (const std::string &name : names_) held = held || strutil::iequals(name, spawns[i].effect);
			known = held;
		}
		if (known) {
			spawns_ = std::move(spawns);
			groups_.resize(spawns_.size(), particle::EffectGroupId());
			statuses_.resize(spawns_.size(), particle::EffectSpawnStatus::InvalidHandle);
			made_.resize(spawns_.size(), false);
			++serial_;
			return false;
		}
	}
	catalog_serial_ = catalog_serial;
	spawns_ = std::move(spawns);
	// Each effect once, in the order the spawns first name it.
	names_.clear();
	for (const DefinitionSpawn &spawn : spawns_) {
		bool held = false;
		for (const std::string &name : names_) held = held || strutil::iequals(name, spawn.effect);
		if (!held) names_.push_back(spawn.effect);
	}
	scene_.reset();
	closures_.clear();
	groups_.assign(spawns_.size(), particle::EffectGroupId());
	statuses_.assign(spawns_.size(), particle::EffectSpawnStatus::InvalidHandle);
	made_.assign(spawns_.size(), false);
	played_ = false;
	++serial_;
	++opens_;
	if (names_.empty()) return true;
	const particle::EffectSceneConfig config = catalog.closures(names_, particle::EffectSceneConfig(), closures_);
	if (config.documents.empty()) return true;
	scene_ = std::make_shared<particle::EffectScene>();
	scene_->open(config);
	return true;
}

void DefinitionEffects::close() {
	if (scene_ || !spawns_.empty()) ++serial_;
	scene_.reset();
	spawns_.clear();
	names_.clear();
	closures_.clear();
	groups_.clear();
	statuses_.clear();
	made_.clear();
	catalog_serial_ = UINT64_MAX;
	played_ = false;
	tick_ = 0;
}

const particle::EffectClosure *DefinitionEffects::closure_of(const std::string &effect) const {
	for (size_t i = 0; i < names_.size() && i < closures_.size(); ++i)
		if (strutil::iequals(names_[i], effect)) return &closures_[i];
	return nullptr;
}

particle::EffectSpawnStatus DefinitionEffects::status(size_t spawn) const {
	return spawn < statuses_.size() ? statuses_[spawn] : particle::EffectSpawnStatus::InvalidHandle;
}

bool DefinitionEffects::alive(size_t spawn) const {
	return scene_ && spawn < groups_.size() && groups_[spawn] && scene_->contains_group(groups_[spawn]);
}

void DefinitionEffects::spawn_(size_t spawn, int32_t age) {
	const DefinitionSpawn &planned = spawns_[spawn];
	particle::EffectSpawnRequest request;
	request.effect = scene_->intern(planned.effect);
	request.pose = planned.pose;
	request.initial_age_ticks = uint32_t(std::max(age, 0));
	const particle::EffectSpawnReceipt receipt = scene_->spawn(request);
	statuses_[spawn] = receipt.status;
	made_[spawn] = true;
	// A spawn whose catch-up outlived every emitter is gone already (EffectScene::spawn releases it).
	groups_[spawn] = receipt.spawned() && scene_->contains_group(receipt.group) ? receipt.group : particle::EffectGroupId();
}

void DefinitionEffects::play_to(int32_t tick) {
	tick = std::max(tick, 0);
	if (!scene_) {
		tick_ = tick;
		return;
	}
	const bool continuous = played_ && tick >= tick_ && tick - tick_ <= int32_t(particle::kEffectAdvanceTickLimit);
	if (!continuous) {
		// A jump: the scene emptied, and each spawn due made again at its age then.
		scene_->reset_runtime_state();
		std::fill(groups_.begin(), groups_.end(), particle::EffectGroupId());
		std::fill(statuses_.begin(), statuses_.end(), particle::EffectSpawnStatus::InvalidHandle);
		std::fill(made_.begin(), made_.end(), false);
		for (size_t i = 0; i < spawns_.size(); ++i)
			if (spawns_[i].tick <= tick) spawn_(i, tick - spawns_[i].tick);
		played_ = true;
		tick_ = tick;
		++serial_;
		return;
	}
	// A spawn added after the clock passed its tick: made at once, pre-aged by its age.
	for (size_t i = 0; i < spawns_.size(); ++i)
		if (!made_[i] && spawns_[i].tick <= tick_) spawn_(i, tick_ - spawns_[i].tick);
	// A game tick at a time (the scene's own fixed step), each spawn made on its tick.
	particle::EffectAdvanceRequest step;
	step.delta_seconds = 1.0f / static_cast<float>(io::kTickHz);
	for (int32_t at = tick_ + 1; at <= tick; ++at) {
		scene_->advance_simulation(step);
		for (size_t i = 0; i < spawns_.size(); ++i)
			if (spawns_[i].tick == at && !made_[i]) spawn_(i, 0);
	}
	if (tick != tick_) ++serial_;
	tick_ = tick;
}

} // namespace opennova::editor
