#include "particle/effect_scene.h"

// [orig: CEffectWorld_InternEffectHandle @ 0x5f7310;
// CEffectWorld_SpawnEmitterAtPosition @ 0x5f6df0;
// CParticleEmitter_AdvanceFrame @ 0x5e6570]

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace opennova::particle {

namespace {

constexpr std::size_t kInvalidIndex = std::numeric_limits<std::size_t>::max();
constexpr std::size_t kDefaultEmitterCapacity = 256;

std::string fold_ascii(std::string_view value) {
	std::string folded;
	folded.reserve(value.size());
	for (const char ch : value) {
		folded.push_back(static_cast<char>(
				std::tolower(static_cast<unsigned char>(ch))));
	}
	return folded;
}

Vec3 add(Vec3 a, Vec3 b) noexcept {
	return {a.x + b.x, a.y + b.y, a.z + b.z};
}

Vec3 scale(Vec3 value, float factor) noexcept {
	return {value.x * factor, value.y * factor, value.z * factor};
}

float length_squared(Vec3 value) noexcept {
	return value.x * value.x + value.y * value.y + value.z * value.z;
}

Vec3 normalized_or_forward(Vec3 value) noexcept {
	const float magnitude_squared = length_squared(value);
	if (!std::isfinite(magnitude_squared) || magnitude_squared <= 1.0e-12f) {
		return {0.0f, 0.0f, 1.0f};
	}
	const float reciprocal = 1.0f / std::sqrt(magnitude_squared);
	return scale(value, reciprocal);
}

Vec3 transform_vector(const EffectPose &pose, Vec3 local) noexcept {
	return {
		pose.right.x * local.x + pose.up.x * local.y + pose.forward.x * local.z,
		pose.right.y * local.x + pose.up.y * local.y + pose.forward.y * local.z,
		pose.right.z * local.x + pose.up.z * local.y + pose.forward.z * local.z,
	};
}

EffectPose compose_pose(const EffectPose &parent, const EffectPose &local) noexcept {
	EffectPose result;
	result.position = add(parent.position, transform_vector(parent, local.position));
	result.right = transform_vector(parent, local.right);
	result.up = transform_vector(parent, local.up);
	result.forward = transform_vector(parent, local.forward);
	return result;
}

void include_point(EffectBounds &bounds, Vec3 point, float radius) noexcept {
	const float safe_radius = std::isfinite(radius) ? std::abs(radius) : 0.0f;
	const Vec3 minimum = {
		point.x - safe_radius,
		point.y - safe_radius,
		point.z - safe_radius,
	};
	const Vec3 maximum = {
		point.x + safe_radius,
		point.y + safe_radius,
		point.z + safe_radius,
	};
	if (!bounds.valid) {
		bounds.minimum = minimum;
		bounds.maximum = maximum;
		bounds.valid = true;
		return;
	}
	bounds.minimum.x = std::min(bounds.minimum.x, minimum.x);
	bounds.minimum.y = std::min(bounds.minimum.y, minimum.y);
	bounds.minimum.z = std::min(bounds.minimum.z, minimum.z);
	bounds.maximum.x = std::max(bounds.maximum.x, maximum.x);
	bounds.maximum.y = std::max(bounds.maximum.y, maximum.y);
	bounds.maximum.z = std::max(bounds.maximum.z, maximum.z);
}

void include_bounds(EffectBounds &into, const EffectBounds &other) noexcept {
	if (!other.valid) {
		return;
	}
	if (!into.valid) {
		into = other;
		return;
	}
	into.minimum.x = std::min(into.minimum.x, other.minimum.x);
	into.minimum.y = std::min(into.minimum.y, other.minimum.y);
	into.minimum.z = std::min(into.minimum.z, other.minimum.z);
	into.maximum.x = std::max(into.maximum.x, other.maximum.x);
	into.maximum.y = std::max(into.maximum.y, other.maximum.y);
	into.maximum.z = std::max(into.maximum.z, other.maximum.z);
}

EffectBounds emitter_bounds(const Emitter &emitter) noexcept {
	EffectBounds bounds;
	for (const Particle &particle : emitter.particles) {
		include_point(bounds, particle.position, particle.size * 0.5f);
	}
	return bounds;
}

bool emitter_finished(const Emitter &emitter) noexcept {
	return emitter.finite && emitter.emit_dur_remaining <= 0.0f &&
			emitter.particles.empty();
}

std::uint32_t seed_for(std::uint32_t base, std::uint64_t group_id,
		std::size_t emitter_ordinal) noexcept {
	std::uint64_t value = static_cast<std::uint64_t>(base) ^
			(group_id + 0x9E3779B97F4A7C15ull +
					(static_cast<std::uint64_t>(emitter_ordinal) << 6u));
	value ^= value >> 30u;
	value *= 0xBF58476D1CE4E5B9ull;
	value ^= value >> 27u;
	value *= 0x94D049BB133111EBull;
	value ^= value >> 31u;
	const std::uint32_t seed = static_cast<std::uint32_t>(value);
	return seed != 0 ? seed : 1u;
}

} // namespace

struct EffectScene::Impl {
	struct CatalogEffect {
		std::string name;
		std::string source;
		std::vector<std::size_t> definition_indices;
	};

	struct InternedEffect {
		std::string name;
		std::size_t catalog_index = kInvalidIndex;
	};

	struct GroupRecord {
		bool active = false;
		EffectGroupId id;
		EffectHandle effect;
		EffectAdmission admission = EffectAdmission::Always;
		EffectBinding binding = EffectBinding::World;
		EffectRenderDomain render_domain = EffectRenderDomain::World;
		EffectSlotToken slot;
		EffectOwnerToken owner;
		EffectPose pose;
		EffectPose owner_relative_pose;
		std::uint64_t source_tick = 0;
		std::uint64_t source_order = 0;
		bool detached = false;
		std::vector<std::size_t> emitter_slots;
	};

	struct EmitterRecord {
		bool active = false;
		std::uint64_t id = 0;
		std::size_t group_slot = kInvalidIndex;
		std::size_t ordinal = 0;
		std::size_t definition_index = 0;
		Emitter emitter;
	};

	EffectSceneConfig config;
	EffectLoadReport load_report;
	std::shared_ptr<const std::vector<ParticleDef>> definitions =
			std::make_shared<std::vector<ParticleDef>>();
	std::vector<CatalogEffect> effects;
	std::unordered_map<std::string, std::size_t> effect_by_name;
	std::size_t stock_effect_index = kInvalidIndex;

	std::vector<InternedEffect> interned;
	std::unordered_map<std::string, EffectHandle> interned_by_name;

	std::vector<GroupRecord> group_pool;
	std::vector<std::size_t> free_group_slots;
	std::vector<std::size_t> active_group_slots;
	std::unordered_map<std::uint64_t, std::size_t> group_by_id;

	std::vector<EmitterRecord> emitter_pool;
	std::vector<std::size_t> free_emitter_slots;

	std::unordered_map<std::uint64_t, std::uint64_t> group_by_slot;
	std::unordered_map<std::uint64_t, EffectPose> owner_poses;

	std::uint64_t next_group_id = 1;
	std::uint64_t next_emitter_id = 1;
	std::uint64_t frame_index = 0;
	double pending_simulation_seconds = 0.0;
	double simulation_time_seconds = 0.0;
	std::size_t suppressed_spawn_count = 0;
	std::size_t rejected_spawn_count = 0;
	std::size_t capacity_rejection_count = 0;

	bool group_capacity_available() const noexcept {
		return !free_group_slots.empty() || config.max_live_groups == 0 ||
				group_pool.size() < config.max_live_groups;
	}

	bool emitter_capacity_available(std::size_t count) const noexcept {
		if (count <= free_emitter_slots.size() || config.max_live_emitters == 0) {
			return true;
		}
		const std::size_t from_pool = count - free_emitter_slots.size();
		return emitter_pool.size() <= config.max_live_emitters &&
				from_pool <= config.max_live_emitters - emitter_pool.size();
	}

	std::size_t acquire_group_slot() {
		std::size_t slot = kInvalidIndex;
		if (!free_group_slots.empty()) {
			slot = free_group_slots.back();
			free_group_slots.pop_back();
		} else {
			slot = group_pool.size();
			group_pool.emplace_back();
		}
		GroupRecord &group = group_pool[slot];
		group.active = true;
		group.id = {};
		group.effect = {};
		group.admission = EffectAdmission::Always;
		group.binding = EffectBinding::World;
		group.render_domain = EffectRenderDomain::World;
		group.slot = {};
		group.owner = {};
		group.pose = {};
		group.owner_relative_pose = {};
		group.source_tick = 0;
		group.source_order = 0;
		group.detached = false;
		group.emitter_slots.clear();
		return slot;
	}

	std::size_t acquire_emitter_slot() {
		std::size_t slot = kInvalidIndex;
		if (!free_emitter_slots.empty()) {
			slot = free_emitter_slots.back();
			free_emitter_slots.pop_back();
		} else {
			slot = emitter_pool.size();
			emitter_pool.emplace_back();
		}
		EmitterRecord &record = emitter_pool[slot];
		record.active = true;
		record.id = next_emitter_id++;
		if (record.id == 0) {
			record.id = next_emitter_id++;
		}
		record.group_slot = kInvalidIndex;
		record.ordinal = 0;
		record.definition_index = 0;
		return slot;
	}

	void release_emitter_slot(std::size_t slot) {
		if (slot >= emitter_pool.size()) {
			return;
		}
		EmitterRecord &record = emitter_pool[slot];
		if (!record.active) {
			return;
		}
		record.active = false;
		record.id = 0;
		record.group_slot = kInvalidIndex;
		record.emitter.active = false;
		record.emitter.def = nullptr;
		record.emitter.particles.clear();
		free_emitter_slots.push_back(slot);
	}

	void reap_finished_emitters(GroupRecord &group) {
		std::size_t write_index = 0;
		for (const std::size_t emitter_slot : group.emitter_slots) {
			const bool keep = emitter_slot < emitter_pool.size() &&
					emitter_pool[emitter_slot].active &&
					!emitter_finished(emitter_pool[emitter_slot].emitter);
			if (keep) {
				group.emitter_slots[write_index++] = emitter_slot;
			} else {
				release_emitter_slot(emitter_slot);
			}
		}
		group.emitter_slots.resize(write_index);
	}

	GroupRecord *find_group(EffectGroupId id) noexcept {
		const auto found = group_by_id.find(id.value);
		if (found == group_by_id.end() || found->second >= group_pool.size()) {
			return nullptr;
		}
		GroupRecord &group = group_pool[found->second];
		return group.active && group.id == id ? &group : nullptr;
	}

	const GroupRecord *find_group(EffectGroupId id) const noexcept {
		const auto found = group_by_id.find(id.value);
		if (found == group_by_id.end() || found->second >= group_pool.size()) {
			return nullptr;
		}
		const GroupRecord &group = group_pool[found->second];
		return group.active && group.id == id ? &group : nullptr;
	}

	void clear_slot_if_owned(const GroupRecord &group) {
		if (!group.slot) {
			return;
		}
		const auto found = group_by_slot.find(group.slot.value);
		if (found != group_by_slot.end() && found->second == group.id.value) {
			group_by_slot.erase(found);
		}
	}

	void detach_group(GroupRecord &group) {
		if (!group.active || group.detached) {
			return;
		}
		clear_slot_if_owned(group);
		group.detached = true;
		group.binding = EffectBinding::World;
		for (const std::size_t emitter_slot : group.emitter_slots) {
			if (emitter_slot >= emitter_pool.size()) {
				continue;
			}
			Emitter &emitter = emitter_pool[emitter_slot].emitter;
			emitter.finite = true;
			emitter.emit_dur_remaining = 0.0f;
		}
	}

	void move_group_to_pose(GroupRecord &group, const EffectPose &pose) {
		group.pose = pose;
		const Vec3 forward = normalized_or_forward(pose.forward);
		for (const std::size_t emitter_slot : group.emitter_slots) {
			if (emitter_slot >= emitter_pool.size()) {
				continue;
			}
			EmitterRecord &record = emitter_pool[emitter_slot];
			if (!record.active) {
				continue;
			}
			emitter_translate(record.emitter, pose.position);
			record.emitter.forward = forward;
		}
	}

	void release_group_slot(std::size_t group_slot) {
		if (group_slot >= group_pool.size()) {
			return;
		}
		GroupRecord &group = group_pool[group_slot];
		if (!group.active) {
			return;
		}
		clear_slot_if_owned(group);
		for (const std::size_t emitter_slot : group.emitter_slots) {
			release_emitter_slot(emitter_slot);
		}
		group.emitter_slots.clear();
		group_by_id.erase(group.id.value);
		group.active = false;
		free_group_slots.push_back(group_slot);
	}

	void reap_finished_groups() {
		std::size_t active_index = 0;
		while (active_index < active_group_slots.size()) {
			const std::size_t group_slot = active_group_slots[active_index];
			GroupRecord &group = group_pool[group_slot];
			reap_finished_emitters(group);
			if (!group.emitter_slots.empty()) {
				++active_index;
				continue;
			}
			release_group_slot(group_slot);
			active_group_slots.erase(active_group_slots.begin() +
					static_cast<std::ptrdiff_t>(active_index));
		}
	}

	std::string interned_name(EffectHandle handle) const {
		if (!handle || handle.value > interned.size()) {
			return {};
		}
		return interned[handle.value - 1u].name;
	}

	const CatalogEffect *catalog_effect(EffectHandle handle) const noexcept {
		if (!handle || handle.value > interned.size()) {
			return nullptr;
		}
		const std::size_t index = interned[handle.value - 1u].catalog_index;
		return index < effects.size() ? &effects[index] : nullptr;
	}

	EffectSpawnReceipt rejected(EffectHandle effect, EffectSpawnStatus status) {
		++rejected_spawn_count;
		if (status == EffectSpawnStatus::GroupCapacityReached ||
				status == EffectSpawnStatus::EmitterCapacityReached) {
			++capacity_rejection_count;
		}
		EffectSpawnReceipt receipt;
		receipt.status = status;
		receipt.effect = effect;
		return receipt;
	}
};

EffectScene::EffectScene() : impl_(std::make_unique<Impl>()) {}

EffectScene::~EffectScene() = default;

EffectScene::EffectScene(EffectScene &&) noexcept = default;

EffectScene &EffectScene::operator=(EffectScene &&) noexcept = default;

EffectLoadReport EffectScene::open(const EffectSceneConfig &config) {
	auto next = std::make_unique<Impl>();
	next->config = config;
	if (!std::isfinite(next->config.simulation_tick_seconds) ||
			next->config.simulation_tick_seconds <=
					std::numeric_limits<float>::epsilon()) {
		next->config.simulation_tick_seconds = 1.0f / 62.5f;
	}
	next->load_report.document_count = config.documents.size();

	if (config.max_live_groups > 0) {
		next->group_pool.reserve(config.max_live_groups);
		next->free_group_slots.reserve(config.max_live_groups);
		next->active_group_slots.reserve(config.max_live_groups);
	}
	if (config.max_live_emitters > 0) {
		next->emitter_pool.reserve(config.max_live_emitters);
		next->free_emitter_slots.reserve(config.max_live_emitters);
	}

	std::vector<TableDef> tables;
	for (const EffectCatalogDocument &document : config.documents) {
		for (const TableDef &table : document.file.tables) {
			tables.push_back(table);
		}
	}
	next->load_report.table_definition_count = tables.size();

	auto mutable_definitions = std::make_shared<std::vector<ParticleDef>>();
	std::unordered_map<std::string, std::size_t> definition_by_name;
	// Particle-def identity and the pdefs member resolve below are
	// case-insensitive like every by-name walk in the effect system
	// [orig: CEffectWorld_FindParticleDefByName @ 0x5e41d0 → _stricmp
	// @ 0x5e420c, called from the EFFDEF→PARDEF resolve @ 0x5e4920].
	for (const EffectCatalogDocument &document : config.documents) {
		for (const ParticleDef &source_definition : document.file.particles) {
			const std::string definition_key = fold_ascii(source_definition.id);
			if (definition_by_name.find(definition_key) !=
					definition_by_name.end()) {
				++next->load_report.duplicate_particle_count;
				continue;
			}
			const std::size_t index = mutable_definitions->size();
			definition_by_name.emplace(definition_key, index);
			mutable_definitions->push_back(source_definition);
			bake_particle_def_curves(mutable_definitions->back(), tables);
		}
	}
	next->load_report.particle_definition_count = mutable_definitions->size();
	next->definitions = mutable_definitions;

	for (const EffectCatalogDocument &document : config.documents) {
		for (const EffectDef &source_effect : document.file.effects) {
			const std::string key = fold_ascii(source_effect.id);
			if (next->effect_by_name.find(key) != next->effect_by_name.end()) {
				++next->load_report.duplicate_effect_count;
				continue;
			}
			Impl::CatalogEffect effect;
			effect.name = source_effect.id;
			effect.source = document.source;
			effect.definition_indices.reserve(source_effect.pdefs.size());
			// The pdefs resolve is all-or-nothing: retail stops at the FIRST
			// missing member, logs it, and CLEARS the effect's whole resolved
			// list — an effect with any missing PARDEF stays registered by
			// name but spawns nothing, never a partial subset
			// [orig: CEffectBank_ResolveAllEntries @ 0x5e4920 — miss breaks
			// @ 0x5e495d, ClearAll @ 0x5e49be, resolved flag stays 0].
			for (const std::string &particle_name : source_effect.pdefs) {
				const auto found = definition_by_name.find(fold_ascii(particle_name));
				if (found == definition_by_name.end()) {
					++next->load_report.unresolved_particle_reference_count;
					effect.definition_indices.clear();
					break;
				}
				effect.definition_indices.push_back(found->second);
			}
			const std::size_t effect_index = next->effects.size();
			next->effect_by_name.emplace(key, effect_index);
			next->effects.push_back(std::move(effect));
		}
	}
	next->load_report.effect_count = next->effects.size();
	const auto stock = next->effect_by_name.find("stockeffect");
	if (stock != next->effect_by_name.end()) {
		next->stock_effect_index = stock->second;
	}
	// The compiled catalog owns every value needed after open. Do not retain
	// the raw parse documents a second time in the scene configuration.
	std::vector<EffectCatalogDocument>().swap(next->config.documents);

	const EffectLoadReport report = next->load_report;
	impl_ = std::move(next);
	return report;
}

EffectHandle EffectScene::intern(std::string_view effect_name_value) {
	const std::string key = fold_ascii(effect_name_value);
	const auto already_interned = impl_->interned_by_name.find(key);
	if (already_interned != impl_->interned_by_name.end()) {
		return already_interned->second;
	}

	std::size_t catalog_index = kInvalidIndex;
	std::string display_name;
	const auto found = impl_->effect_by_name.find(key);
	if (found != impl_->effect_by_name.end()) {
		catalog_index = found->second;
		display_name = impl_->effects[catalog_index].name;
	} else if (impl_->stock_effect_index != kInvalidIndex) {
		catalog_index = impl_->stock_effect_index;
		display_name.assign(effect_name_value.begin(), effect_name_value.end());
	} else {
		return {};
	}

	if (impl_->interned.size() >=
			static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
		return {};
	}
	Impl::InternedEffect entry;
	entry.name = std::move(display_name);
	entry.catalog_index = catalog_index;
	impl_->interned.push_back(std::move(entry));
	const EffectHandle handle{
		static_cast<std::uint32_t>(impl_->interned.size())
	};
	impl_->interned_by_name.emplace(key, handle);
	return handle;
}

std::string EffectScene::effect_name(EffectHandle handle) const {
	return impl_->interned_name(handle);
}

EffectSpawnReceipt EffectScene::spawn(const EffectSpawnRequest &request) {
	const Impl::CatalogEffect *effect = impl_->catalog_effect(request.effect);
	if (effect == nullptr) {
		return impl_->rejected(request.effect, EffectSpawnStatus::InvalidHandle);
	}
	if ((request.admission == EffectAdmission::ReplaceOwned ||
				request.admission == EffectAdmission::SuppressWhileOwned) &&
			!request.slot) {
		return impl_->rejected(request.effect, EffectSpawnStatus::MissingSlot);
	}
	if (request.binding == EffectBinding::FollowOwner && !request.owner) {
		return impl_->rejected(request.effect, EffectSpawnStatus::MissingOwner);
	}
	if (effect->definition_indices.empty()) {
		return impl_->rejected(request.effect, EffectSpawnStatus::EmptyEffect);
	}

	const bool uses_slot = request.admission != EffectAdmission::Always;
	EffectGroupId previous_group;
	if (uses_slot) {
		const auto occupied = impl_->group_by_slot.find(request.slot.value);
		if (occupied != impl_->group_by_slot.end()) {
			previous_group.value = occupied->second;
			Impl::GroupRecord *previous = impl_->find_group(previous_group);
			if (previous == nullptr || previous->detached) {
				impl_->group_by_slot.erase(occupied);
				previous_group = {};
			} else if (request.admission == EffectAdmission::SuppressWhileOwned) {
				++impl_->suppressed_spawn_count;
				EffectSpawnReceipt receipt;
				receipt.status = EffectSpawnStatus::Suppressed;
				receipt.effect = request.effect;
				receipt.group = previous_group;
				return receipt;
			}
		}
	}

	if (!impl_->group_capacity_available()) {
		return impl_->rejected(
				request.effect, EffectSpawnStatus::GroupCapacityReached);
	}
	if (!impl_->emitter_capacity_available(effect->definition_indices.size())) {
		return impl_->rejected(
				request.effect, EffectSpawnStatus::EmitterCapacityReached);
	}

	if (request.admission == EffectAdmission::ReplaceOwned && previous_group) {
		Impl::GroupRecord *previous = impl_->find_group(previous_group);
		if (previous != nullptr) {
			impl_->detach_group(*previous);
		}
	}

	const std::size_t group_slot = impl_->acquire_group_slot();
	Impl::GroupRecord &group = impl_->group_pool[group_slot];
	group.id.value = impl_->next_group_id++;
	if (group.id.value == 0) {
		group.id.value = impl_->next_group_id++;
	}
	group.effect = request.effect;
	group.admission = request.admission;
	group.binding = request.binding;
	group.render_domain = request.render_domain;
	group.slot = uses_slot ? request.slot : EffectSlotToken{};
	group.owner = request.owner;
	group.pose = request.pose;
	group.owner_relative_pose = request.owner_relative_pose;
	group.source_tick = request.source_tick;
	group.source_order = request.source_order;
	group.emitter_slots.reserve(effect->definition_indices.size());

	if (group.binding == EffectBinding::FollowOwner) {
		const auto owner_pose = impl_->owner_poses.find(group.owner.value);
		if (owner_pose != impl_->owner_poses.end()) {
			group.pose = compose_pose(owner_pose->second, group.owner_relative_pose);
		}
	}

	for (std::size_t ordinal = 0;
			ordinal < effect->definition_indices.size(); ++ordinal) {
		const std::size_t definition_index = effect->definition_indices[ordinal];
		const ParticleDef &definition = (*impl_->definitions)[definition_index];
		const std::size_t emitter_slot = impl_->acquire_emitter_slot();
		Impl::EmitterRecord &record = impl_->emitter_pool[emitter_slot];
		record.group_slot = group_slot;
		record.ordinal = ordinal;
		record.definition_index = definition_index;
		Emitter &emitter = record.emitter;
		emitter.max_particles = definition.emit_maxoverride > 0
				? std::min(static_cast<std::size_t>(definition.emit_maxoverride),
						kEmitterHardParticleLimit)
				: kDefaultEmitterCapacity;
		emitter.color_tint = request.color_tint;
		emitter.spring_const = request.spring_const;
		emitter.lod_divisor = std::max(request.lod_divisor, 1u);
		EffectKillPlane kill_plane = request.kill_plane;
		if (kill_plane == EffectKillPlane::Disabled) {
			if ((definition.flags & particle_flag::BelowH2O) != 0) {
				kill_plane = EffectKillPlane::KillAbove;
			} else if ((definition.flags & particle_flag::AboveH2O) != 0) {
				kill_plane = EffectKillPlane::KillAtOrBelow;
			}
		}
		emitter.kill_plane_mode = static_cast<std::uint32_t>(kill_plane);
		emitter.kill_plane_y = request.kill_plane_y;
		emitter_init(emitter, &definition, group.pose.position,
				seed_for(impl_->config.random_seed, group.id.value, ordinal));
		emitter.forward = normalized_or_forward(group.pose.forward);
		group.emitter_slots.push_back(emitter_slot);
	}

	const std::uint32_t initial_age_ticks = std::min(
			request.initial_age_ticks, kEffectInitialAgeTickLimit);
	for (std::uint32_t tick = 0; tick < initial_age_ticks; ++tick) {
		for (const std::size_t emitter_slot : group.emitter_slots) {
			Emitter &emitter = impl_->emitter_pool[emitter_slot].emitter;
			emitter_advance(emitter,
					impl_->config.simulation_tick_seconds);
		}
		impl_->reap_finished_emitters(group);
		if (group.emitter_slots.empty()) {
			break;
		}
	}

	const EffectGroupId spawned_group = group.id;
	if (group.emitter_slots.empty()) {
		// Initial-age replay can exhaust every child before the group is
		// published. Treat it as already destroyed so it consumes neither pool
		// capacity nor a suppress-while-owned slot.
		impl_->release_group_slot(group_slot);
	} else {
		impl_->active_group_slots.push_back(group_slot);
		impl_->group_by_id.emplace(group.id.value, group_slot);
		if (group.slot) {
			impl_->group_by_slot[group.slot.value] = group.id.value;
		}
	}

	EffectSpawnReceipt receipt;
	receipt.status = EffectSpawnStatus::Spawned;
	receipt.effect = request.effect;
	receipt.group = spawned_group;
	if (request.admission == EffectAdmission::ReplaceOwned) {
		receipt.replaced_group = previous_group;
	}
	return receipt;
}

void EffectScene::apply_owner_poses(
		const std::vector<EffectOwnerPoseUpdate> &updates) {
	std::unordered_map<std::uint64_t, std::vector<std::size_t>> groups_by_owner;
	groups_by_owner.reserve(impl_->active_group_slots.size());
	for (const std::size_t group_slot : impl_->active_group_slots) {
		const Impl::GroupRecord &group = impl_->group_pool[group_slot];
		if (group.active && group.binding == EffectBinding::FollowOwner &&
				group.owner) {
			groups_by_owner[group.owner.value].push_back(group_slot);
		}
	}
	for (const EffectOwnerPoseUpdate &update : updates) {
		if (!update.owner) {
			continue;
		}
		if (!update.present) {
			impl_->owner_poses.erase(update.owner.value);
			const auto groups = groups_by_owner.find(update.owner.value);
			if (groups == groups_by_owner.end()) {
				continue;
			}
			for (const std::size_t group_slot : groups->second) {
				Impl::GroupRecord &group = impl_->group_pool[group_slot];
				if (group.active && group.binding == EffectBinding::FollowOwner &&
						group.owner.value == update.owner.value) {
					impl_->detach_group(group);
				}
			}
			continue;
		}

		impl_->owner_poses[update.owner.value] = update.pose;
		const auto groups = groups_by_owner.find(update.owner.value);
		if (groups == groups_by_owner.end()) {
			continue;
		}
		for (const std::size_t group_slot : groups->second) {
			Impl::GroupRecord &group = impl_->group_pool[group_slot];
			if (!group.active || group.binding != EffectBinding::FollowOwner ||
					group.owner.value != update.owner.value) {
				continue;
			}
			impl_->move_group_to_pose(
					group, compose_pose(update.pose, group.owner_relative_pose));
		}
	}
}

std::vector<EffectOwnerToken> EffectScene::active_owner_tokens() const {
	std::vector<EffectOwnerToken> result;
	std::unordered_set<std::uint64_t> seen;
	for (const std::size_t group_slot : impl_->active_group_slots) {
		const Impl::GroupRecord &group = impl_->group_pool[group_slot];
		if (!group.active || group.detached ||
				group.binding != EffectBinding::FollowOwner || !group.owner ||
				!seen.insert(group.owner.value).second) {
			continue;
		}
		result.push_back(group.owner);
	}
	return result;
}

void EffectScene::detach(EffectGroupId group_id) {
	Impl::GroupRecord *group = impl_->find_group(group_id);
	if (group != nullptr) {
		impl_->detach_group(*group);
	}
}

void EffectScene::detach_slot(EffectSlotToken slot) {
	if (!slot) {
		return;
	}
	const auto found = impl_->group_by_slot.find(slot.value);
	if (found == impl_->group_by_slot.end()) {
		return;
	}
	Impl::GroupRecord *group = impl_->find_group(EffectGroupId{found->second});
	if (group != nullptr) {
		impl_->detach_group(*group);
	} else {
		impl_->group_by_slot.erase(found);
	}
}

void EffectScene::reset_runtime_state() {
	// Destroy live values but retain the vectors' outer allocations. Their
	// sizes are also the pool high-water diagnostics, so a restarted session
	// begins with clean counters while the next spawn can reuse capacity.
	impl_->active_group_slots.clear();
	impl_->group_by_id.clear();
	impl_->group_by_slot.clear();
	impl_->owner_poses.clear();
	impl_->free_group_slots.clear();
	impl_->free_emitter_slots.clear();
	impl_->group_pool.clear();
	impl_->emitter_pool.clear();

	impl_->next_group_id = 1;
	impl_->next_emitter_id = 1;
	impl_->frame_index = 0;
	impl_->pending_simulation_seconds = 0.0;
	impl_->simulation_time_seconds = 0.0;
	impl_->suppressed_spawn_count = 0;
	impl_->rejected_spawn_count = 0;
	impl_->capacity_rejection_count = 0;
}

void EffectScene::advance_simulation(const EffectAdvanceRequest &request) {
	double requested_seconds = static_cast<double>(request.delta_seconds);
	if (!std::isfinite(requested_seconds) || requested_seconds < 0.0) {
		requested_seconds = 0.0;
	}
	const double fixed_step =
			static_cast<double>(impl_->config.simulation_tick_seconds);
	constexpr double kStepEpsilon = 1.0e-12;
	const double accumulated_seconds =
			impl_->pending_simulation_seconds + requested_seconds;
	const double available_steps = std::floor(
			(accumulated_seconds + kStepEpsilon) / fixed_step);
	std::uint32_t step_count = 0;
	if (available_steps > static_cast<double>(kEffectAdvanceTickLimit)) {
		step_count = kEffectAdvanceTickLimit;
		impl_->pending_simulation_seconds =
				std::fmod(accumulated_seconds, fixed_step);
		// fmod can land infinitesimally below the divisor for very large input.
		// Treat that as the same fixed-step boundary used above, not as deferred
		// work for a later zero-delta call.
		if (impl_->pending_simulation_seconds + kStepEpsilon >= fixed_step) {
			impl_->pending_simulation_seconds = 0.0;
		}
	} else if (available_steps > 0.0) {
		step_count = static_cast<std::uint32_t>(available_steps);
		impl_->pending_simulation_seconds = accumulated_seconds -
				static_cast<double>(step_count) * fixed_step;
	} else {
		impl_->pending_simulation_seconds = accumulated_seconds;
	}
	if (impl_->pending_simulation_seconds < 0.0) {
		impl_->pending_simulation_seconds = 0.0;
	}

	for (std::uint32_t step = 0; step < step_count; ++step) {
		for (const std::size_t group_slot : impl_->active_group_slots) {
			Impl::GroupRecord &group = impl_->group_pool[group_slot];
			for (const std::size_t emitter_slot : group.emitter_slots) {
				Impl::EmitterRecord &record = impl_->emitter_pool[emitter_slot];
				if (record.active) {
					emitter_advance(
							record.emitter, impl_->config.simulation_tick_seconds);
				}
			}
		}
		// Retail advances and destroys dead children individually. The admission
		// slot remains attached to the group and is cleared only when the final
		// child is gone (CEffectGroup_Destroy's group death callback).
		impl_->reap_finished_groups();
		impl_->simulation_time_seconds += fixed_step;
	}
	++impl_->frame_index;
}

void EffectScene::write_snapshot(ParticleFrameSnapshot &snapshot) const {
	snapshot.frame_index = impl_->frame_index;
	snapshot.simulation_time_seconds = impl_->simulation_time_seconds;
	snapshot.definitions = impl_->definitions;
	snapshot.groups.clear();
	snapshot.emitters.clear();
	snapshot.particles.clear();
	snapshot.groups.reserve(impl_->active_group_slots.size());

	std::size_t emitter_count = 0;
	std::size_t particle_count = 0;
	for (const std::size_t group_slot : impl_->active_group_slots) {
		const Impl::GroupRecord &group = impl_->group_pool[group_slot];
		emitter_count += group.emitter_slots.size();
		for (const std::size_t emitter_slot : group.emitter_slots) {
			particle_count +=
					impl_->emitter_pool[emitter_slot].emitter.particles.size();
		}
	}
	snapshot.emitters.reserve(emitter_count);
	snapshot.particles.reserve(particle_count);

	for (const std::size_t group_slot : impl_->active_group_slots) {
		const Impl::GroupRecord &group = impl_->group_pool[group_slot];
		EffectGroupFrameSnapshot group_snapshot;
		group_snapshot.id = group.id;
		group_snapshot.effect = group.effect;
		group_snapshot.effect_name = impl_->interned_name(group.effect);
		const Impl::CatalogEffect *catalog_effect =
				impl_->catalog_effect(group.effect);
		if (catalog_effect != nullptr) {
			group_snapshot.source = catalog_effect->source;
		}
		group_snapshot.pose = group.pose;
		group_snapshot.render_domain = group.render_domain;
		group_snapshot.source_tick = group.source_tick;
		group_snapshot.source_order = group.source_order;
		group_snapshot.first_emitter = snapshot.emitters.size();
		group_snapshot.emitter_count = group.emitter_slots.size();
		group_snapshot.detached = group.detached;
		const std::size_t group_index = snapshot.groups.size();
		snapshot.groups.push_back(std::move(group_snapshot));

		for (const std::size_t emitter_slot : group.emitter_slots) {
			const Impl::EmitterRecord &record = impl_->emitter_pool[emitter_slot];
			const Emitter &emitter = record.emitter;
			EffectEmitterFrameSnapshot emitter_snapshot;
			emitter_snapshot.id = record.id;
			emitter_snapshot.group_index = group_index;
			emitter_snapshot.ordinal = record.ordinal;
			emitter_snapshot.definition_index = record.definition_index;
			emitter_snapshot.first_particle = snapshot.particles.size();
			emitter_snapshot.particle_count = emitter.particles.size();
			emitter_snapshot.position = emitter.position;
			emitter_snapshot.forward = emitter.forward;
			emitter_snapshot.color_tint = emitter.color_tint;
			emitter_snapshot.age = emitter.age;
			emitter_snapshot.spring_const = emitter.spring_const;
			emitter_snapshot.lod_divisor = emitter.lod_divisor;
			emitter_snapshot.kill_plane =
					static_cast<EffectKillPlane>(emitter.kill_plane_mode);
			emitter_snapshot.kill_plane_y = emitter.kill_plane_y;
			snapshot.emitters.push_back(std::move(emitter_snapshot));
			snapshot.particles.insert(snapshot.particles.end(),
					emitter.particles.begin(), emitter.particles.end());
		}
	}
}

ParticleFrameSnapshot EffectScene::advance(const EffectAdvanceRequest &request) {
	advance_simulation(request);
	ParticleFrameSnapshot snapshot;
	write_snapshot(snapshot);
	return snapshot;
}

EffectLiveCounts EffectScene::live_counts() const noexcept {
	EffectLiveCounts counts;
	counts.group_count = impl_->active_group_slots.size();
	for (const std::size_t group_slot : impl_->active_group_slots) {
		const Impl::GroupRecord &group = impl_->group_pool[group_slot];
		for (const std::size_t emitter_slot : group.emitter_slots) {
			const Impl::EmitterRecord &record = impl_->emitter_pool[emitter_slot];
			if (!record.active)
				continue;
			++counts.emitter_count;
			counts.particle_count += record.emitter.particles.size();
		}
	}
	return counts;
}

EffectDebugSnapshot EffectScene::inspect(bool p_include_bounds) const {
	EffectDebugSnapshot snapshot;
	snapshot.load = impl_->load_report;
	snapshot.interned_effect_count = impl_->interned.size();
	snapshot.live_group_count = impl_->active_group_slots.size();
	snapshot.group_pool_high_water = impl_->group_pool.size();
	snapshot.emitter_pool_high_water = impl_->emitter_pool.size();
	snapshot.suppressed_spawn_count = impl_->suppressed_spawn_count;
	snapshot.rejected_spawn_count = impl_->rejected_spawn_count;
	snapshot.capacity_rejection_count = impl_->capacity_rejection_count;
	snapshot.groups.reserve(impl_->active_group_slots.size());

	for (const std::size_t group_slot : impl_->active_group_slots) {
		const Impl::GroupRecord &group = impl_->group_pool[group_slot];
		EffectGroupDebugSnapshot group_snapshot;
		group_snapshot.id = group.id;
		group_snapshot.effect = group.effect;
		group_snapshot.effect_name = impl_->interned_name(group.effect);
		const Impl::CatalogEffect *catalog_effect =
				impl_->catalog_effect(group.effect);
		if (catalog_effect != nullptr) {
			group_snapshot.source = catalog_effect->source;
		}
		group_snapshot.admission = group.admission;
		group_snapshot.binding = group.binding;
		group_snapshot.render_domain = group.render_domain;
		group_snapshot.slot = group.slot;
		group_snapshot.owner = group.owner;
		group_snapshot.detached = group.detached;
		group_snapshot.pose = group.pose;
		group_snapshot.source_tick = group.source_tick;
		group_snapshot.source_order = group.source_order;
		group_snapshot.emitters.reserve(group.emitter_slots.size());

		for (const std::size_t emitter_slot : group.emitter_slots) {
			const Impl::EmitterRecord &record = impl_->emitter_pool[emitter_slot];
			if (!record.active) {
				continue;
			}
			const Emitter &emitter = record.emitter;
			EffectEmitterDebugSnapshot emitter_snapshot;
			emitter_snapshot.id = record.id;
			emitter_snapshot.ordinal = record.ordinal;
			emitter_snapshot.definition_index = record.definition_index;
			if (record.definition_index < impl_->definitions->size()) {
				const ParticleDef &definition =
						(*impl_->definitions)[record.definition_index];
				emitter_snapshot.definition_name = definition.id;
				emitter_snapshot.definition_flags = definition.flags;
			}
			emitter_snapshot.alive_particle_count = emitter.particles.size();
			emitter_snapshot.emitting = !group.detached && emitter.active &&
					(!emitter.finite || emitter.emit_dur_remaining > 0.0f);
			emitter_snapshot.position = emitter.position;
			emitter_snapshot.forward = emitter.forward;
			emitter_snapshot.age = emitter.age;
			emitter_snapshot.kill_plane =
					static_cast<EffectKillPlane>(emitter.kill_plane_mode);
			emitter_snapshot.kill_plane_y = emitter.kill_plane_y;
			if (p_include_bounds) {
				emitter_snapshot.bounds = emitter_bounds(emitter);
				include_bounds(group_snapshot.bounds, emitter_snapshot.bounds);
			}
			snapshot.live_particle_count += emitter.particles.size();
			++snapshot.live_emitter_count;
			group_snapshot.emitters.push_back(std::move(emitter_snapshot));
		}
		snapshot.groups.push_back(std::move(group_snapshot));
	}
	return snapshot;
}

} // namespace opennova::particle
