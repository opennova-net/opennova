#include <runtime/world/facial_animation.h>

#include <base/io/bam.h>
#include <base/io/strutil.h>
#include <base/resource_index/resource_index.h>
#include <formats/def/def.h>
#include <runtime/world/world.h>

#include <algorithm>
#include <chrono>
#include <numeric>
#include <utility>

namespace opennova::world {
namespace {

std::string grm_name(std::string name) {
	// ItemDef+96 is the graphic name, not a material/texture name.
	// [orig: Entity_InitFromModel @0x40E229; sub_57FCE0]
	const size_t slash = name.find_last_of("/\\");
	if (slash != std::string::npos) name.erase(0, slash + 1);
	const size_t dot = name.find_last_of('.');
	if (dot != std::string::npos) name.erase(dot);
	return name + ".GRM";
}

const grm::Gesture *gesture(const grm::File &file, int32_t expression) {
	if (expression < 0 || expression >= 9) return nullptr;
	const grm::Gesture *result = nullptr;
	for (const auto &g : file.gestures)
		if (strutil::iequals(g.name, kFacialExpressions[expression])) result = &g;
	return result; // last match [orig: sub_588FE0]
}

grm::Point offset(const grm::Gesture *g, const std::string &name) {
	if (g)
		for (const auto &p : g->parameters)
			if (strutil::iequals(p.group, name)) return p.offset;
	return {};
}

void position_of(const World &world, const Entity &entity, int32_t out[2]) {
	if (const AiEntity *body = world.ai.for_handle(entity.handle)) {
		out[0] = body->pos[0]; out[1] = body->pos[1];
	} else {
		out[0] = static_cast<int32_t>(entity.position.x * 65536.0f);
		out[1] = static_cast<int32_t>(entity.position.y * 65536.0f);
	}
}

} // namespace

int facial_expression_index(const std::string &name) {
	// [orig: AnimState_FindByName @0x5800B0]
	for (int i = 0; i < 9; ++i)
		if (strutil::iequals(name, kFacialExpressions[i])) return i;
	return -1;
}

void step_facial_animation(FacialSlot &slot, bool dead, io::CrtRand &random,
		uint32_t display_frame, uint32_t wall_time_ms) {
	// [orig: scar_decal_update @0x57FA50]
	slot.blend += 0.125f;
	if ((display_frame & 63u) == 0) {
		slot.random_eyes.x = static_cast<float>((int32_t(random.next()) - 16384) * 0.000061035156);
		slot.random_eyes.y = static_cast<float>((int32_t(random.next()) - 16384) * 0.000061035156);
	}
	if (slot.blend >= 1.0f) {
		slot.current = slot.next;
		slot.blend = 0.0f;
		if (dead) slot.next = 5;
		else if (slot.expression_override == -1) slot.next = slot.automatic;
		else {
			slot.override_timer = io::bam_sub(slot.override_timer, 1);
			slot.next = slot.expression_override;
			if (slot.override_timer < 0) slot.expression_override = -1;
		}
		if (slot.next == -1) slot.next = static_cast<int32_t>((wall_time_ms >> 10) % 9);
	}
	if (dead) {
		slot.eyes.x = static_cast<float>(-slot.eyes.x * 0.050000001 + slot.eyes.x);
		slot.eyes.y = static_cast<float>(0.050000001 * (-2.2 - slot.eyes.y) + slot.eyes.y);
	} else {
		grm::Point target = slot.random_eyes;
		if (slot.directed_timer != 0) {
			slot.directed_timer = io::bam_sub(slot.directed_timer, 1);
			target = slot.directed_eyes;
		}
		slot.eyes.x = static_cast<float>((target.x - slot.eyes.x) * 0.25 + slot.eyes.x);
		slot.eyes.y = static_cast<float>((target.y - slot.eyes.y) * 0.25 + slot.eyes.y);
	}
	if (slot.current == slot.next) slot.blend = 1.0f;
}

std::vector<grm::Point> evaluate_facial_mesh(const grm::File &file,
		int32_t current, int32_t next, float blend) {
	const auto *a = gesture(file, current), *b = gesture(file, next);
	if (!a) a = b;
	if (!b) b = a;
	std::vector<grm::Point> positions;
	positions.reserve(file.vertices.size());
	for (const auto &v : file.vertices) {
		grm::Point p = v.uv;
		// "xxx" is the group-zero sentinel even when authored in a gesture.
		// [orig: collect_unique_material_names @0x588D90; sub_5890F0]
		if (!strutil::iequals(v.group, "xxx")) {
			const auto da = offset(a, v.group), db = offset(b, v.group);
			p.x = static_cast<float>((1.0 - blend) * da.x + blend * db.x + p.x);
			p.y = static_cast<float>((1.0 - blend) * da.y + blend * db.y + p.y);
		}
		positions.push_back(p);
	}
	return positions;
}

void FacialSystem::configure(World &world, const ResourceIndex *index,
		const def::DefItemsFile &items) {
	if (index_ != index) {
		index_ = index;
		models_.clear();
		slots_.clear();
		load_errors_.clear();
		display_frame_ = 0;
		world.registry.for_each([&](const Entity &old) {
			Entity *e = world.registry.get(old.handle);
			e->facial_slot = 0;
			e->facial_checked = false;
		});
	}
	model_names_.clear();
	for (size_t i = 0; i < items.count; ++i) {
		const auto &row = items.entries[i];
		if (row.type == def::DEF_ITEM_TYPE_PERSON && row.graphic[0])
			model_names_[row.id - 100000] = grm_name(row.graphic);
	}
	initialize(world);
}

FacialSlot *FacialSystem::for_entity(const Entity &entity) {
	const size_t i = entity.facial_slot & 0x7FFF;
	if (entity.facial_slot == 0 || i >= slots_.size()) return nullptr;
	FacialSlot &slot = slots_[i];
	return slot.owner == entity.handle && slot.spawn_id == entity.registry_spawn_id ? &slot : nullptr;
}

const FacialSlot *FacialSystem::for_entity(const Entity &entity) const {
	return const_cast<FacialSystem *>(this)->for_entity(entity);
}

void FacialSystem::initialize_entity(World &world, Entity &entity) {
	if (for_entity(entity)) return;
	if (entity.facial_slot) {
		entity.facial_slot = 0;
		entity.facial_checked = false;
	}
	if (entity.facial_checked || !entity.has_item_def || entity.item_type != 3 || !index_) return;
	entity.facial_checked = true;
	if (slots_.size() >= kCapacity) return;
	const auto binding = model_names_.find(entity.item_id);
	if (binding == model_names_.end()) return;
	std::shared_ptr<const grm::File> model;
	for (const auto &entry : models_)
		if (strutil::iequals(entry.first, binding->second)) { model = entry.second; break; }
	if (!model) {
		if (models_.size() >= kModelCapacity) return;
		std::vector<uint8_t> bytes;
		if (!index_->read_file(binding->second, bytes)) return;
		auto parsed = std::make_shared<grm::File>();
		std::string error;
		if (!grm::parse(bytes.data(), bytes.size(), *parsed, error)) {
			const std::string message = binding->second + ": " + error;
			if (std::find(load_errors_.begin(), load_errors_.end(), message) == load_errors_.end())
				load_errors_.push_back(message);
			return;
		}
		model = std::move(parsed);
		models_.emplace_back(binding->second, model);
	}
	// Original allocation is append-only, including after a slot is freed.
	// [orig: sub_57FDF0 @0x57FDF0; sub_57FCA0 @0x57FCA0]
	FacialSlot slot;
	slot.owner = entity.handle;
	slot.spawn_id = entity.registry_spawn_id;
	slot.model = std::move(model);
	position_of(world, entity, slot.position);
	entity.facial_slot = static_cast<uint16_t>(slots_.size() | 0x8000u);
	slots_.push_back(std::move(slot));
}

// [orig: entity+440 slot release @0x57FCA0, called by Entity_Destroy]
void FacialSystem::release(Entity &entity) {
	if (FacialSlot *slot = for_entity(entity)) *slot = FacialSlot{};
	entity.facial_slot = 0;
}

void FacialSystem::initialize(World &world) {
	for (auto &slot : slots_) {
		const Entity *entity = world.registry.get(slot.owner);
		if (entity && entity->registry_spawn_id == slot.spawn_id) continue;
		slot = FacialSlot{};
	}
	world.registry.for_each([&](const Entity &row) {
		initialize_entity(world, *world.registry.get(row.handle));
	});
}

void FacialSystem::tick(World &world) {
	initialize(world);
	const uint32_t now_ms = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());
	for (auto &slot : slots_) {
		const Entity *entity = world.registry.get(slot.owner);
		if (!entity || entity->registry_spawn_id != slot.spawn_id) continue;
		step_facial_animation(slot, ((entity->flags | entity->engine_flags) & 2u) != 0,
				world.crt_rand, display_frame_, now_ms);
		// [orig: sub_57FFD0 @0x57FFD0, caller @0x4C2617]
		position_of(world, *entity, slot.position);
	}
}

void FacialSystem::override_expression(const Entity &entity, int32_t expression, int32_t transitions) {
	if (FacialSlot *slot = for_entity(entity)) {
		slot->expression_override = expression;
		slot->override_timer = transitions;
	}
}

void FacialSystem::automatic_expression(const Entity &entity, int32_t expression) {
	if (FacialSlot *slot = for_entity(entity)) slot->automatic = expression;
	// [orig: PlayerSlot_SetTimeout @0x4AD4C0 — misnamed: GRM slot+52]
}

void FacialSystem::compile_draws(World &world, int32_t camera_x, int32_t camera_y,
		bool first_person, std::vector<FacialDraw> &out) {
	out.clear();
	++display_frame_;
	std::vector<size_t> order(slots_.size());
	std::iota(order.begin(), order.end(), 0);
	for (auto &slot : slots_) {
		const Entity *entity = world.registry.get(slot.owner);
		if (entity && entity->registry_spawn_id == slot.spawn_id) position_of(world, *entity, slot.position);
		const int32_t dx = io::bam_abs(io::bam_sub(slot.position[0], camera_x));
		const int32_t dy = io::bam_abs(io::bam_sub(slot.position[1], camera_y));
		slot.distance = io::bam_add(std::max(dx, dy), std::min(dx, dy) >> 1);
		if (!entity || entity->registry_spawn_id != slot.spawn_id ||
				((entity->flags | entity->engine_flags) & 1u) != 0 ||
				(entity->handle == world.cached.local_player && first_person))
			slot.distance = 0x40000000;
	}
	std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
		return slots_[a].distance < slots_[b].distance;
	});
	int priority = 0;
	for (size_t i : order) {
		auto &slot = slots_[i];
		if (priority >= 3 || slot.distance >= 0x40000000) {
			slot.active = false; slot.priority = -1; slot.priority_changed = false;
			continue;
		}
		slot.priority_changed = slot.priority != priority;
		slot.priority = priority++;
		slot.active = true;
		const Entity *entity = world.registry.get(slot.owner);
		FacialDraw draw;
		draw.owner = slot.owner;
		draw.spawn_id = slot.spawn_id;
		draw.bms_id = entity->bms_id;
		draw.spawn_origin = entity->spawn_origin;
		draw.texture_slot = static_cast<uint8_t>(slot.priority);
		draw.resolution = 256 >> slot.priority;
		draw.redraw = slot.owner == world.cached.local_player ||
				((display_frame_ ^ i) & 3u) == 0 || slot.priority_changed || slot.distance <= 81920;
		draw.model = slot.model;
		draw.positions = evaluate_facial_mesh(*slot.model, slot.current, slot.next, slot.blend);
		draw.eye_offset = {slot.eyes.x * 0.02f, slot.eyes.y * 0.01f};
		out.push_back(std::move(draw));
	}
}

} // namespace opennova::world
