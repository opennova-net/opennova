// Mission-owned facial slots and GRM texture animation.
// [orig: CScarDecal_Init @0x57F900; sub_57FDF0; CScarDecal_Update @0x57FA50]
#pragma once

#include <formats/grm/grm.h>
#include <runtime/world/entity.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace opennova { class ResourceIndex; }
namespace opennova::def { struct DefItemsFile; }

namespace opennova::world {

class World;

inline constexpr const char *kFacialExpressions[9] = {
	"NORMAL", "HAPPY", "SAD", "SMIRK", "ANGRY", "SURPRISE", "DISGUST", "FEAR", "AGGRESSIVE"
}; // [orig: name table @0x7D7960, 64-byte rows]
int facial_expression_index(const std::string &name);

struct FacialSlot {
	EntityHandle owner;
	uint64_t spawn_id = 0;
	std::shared_ptr<const grm::File> model;
	int32_t current = 0, next = 0;
	float blend = 0.0f;
	int32_t expression_override = -1, override_timer = 0, automatic = -1;
	grm::Point eyes, random_eyes, directed_eyes;
	int32_t directed_timer = 0;
	int32_t position[2] = {};
	bool active = false, priority_changed = false;
	int32_t priority = 0, distance = 0;
};

// Separates the original simulation counter, display-frame counter and wall
// clock. The first controls calls, the latter two random gaze and idle state.
// The gaze pair is gated on the display counter, so it draws from the
// thread-local render/effects CRT stream (base/crt/crt_rng.h), never from the
// session-seeded World::crt_rand (D-NET-115).
// [orig: CScarDecal_Update @0x57FA50, caller @0x4C21FB]
void step_facial_animation(FacialSlot &slot, bool dead, uint32_t display_frame,
		uint32_t wall_time_ms);

// Position of every GRM mesh vertex after current/next gesture interpolation.
// The texture coordinates themselves stay at the base mesh UVs.
// [orig: sub_588FE0, sub_589090, sub_5890F0]
std::vector<grm::Point> evaluate_facial_mesh(const grm::File &file,
		int32_t current, int32_t next, float blend);

struct FacialDraw {
	EntityHandle owner;
	uint64_t spawn_id = 0;
	int32_t bms_id = -1;
	uint32_t spawn_origin = kSpawnOriginNone;
	uint8_t texture_slot = 0;
	int resolution = 256;
	bool redraw = false;
	std::shared_ptr<const grm::File> model;
	std::vector<grm::Point> positions;
	grm::Point eye_offset;
};

class FacialSystem {
public:
	static constexpr size_t kCapacity = 256;
	static constexpr size_t kModelCapacity = 64;

	void configure(World &world, const ResourceIndex *index, const def::DefItemsFile &items);
	void initialize(World &world);
	void release(Entity &entity);
	void tick(World &world);
	FacialSlot *for_entity(const Entity &entity);
	const FacialSlot *for_entity(const Entity &entity) const;
	void override_expression(const Entity &entity, int32_t expression, int32_t transitions = 80);
	void automatic_expression(const Entity &entity, int32_t expression);

	// Stable nearest-first sort; only three visible slots have live targets.
	// [orig: Scar_SortSlotsByDistance @0x57FE60; sub_580360; Render_ScarSlot @0x580170]
	void compile_draws(World &world, int32_t camera_x, int32_t camera_y,
			bool first_person, std::vector<FacialDraw> &out);

	const std::vector<FacialSlot> &slots() const { return slots_; }
	uint32_t display_frame() const { return display_frame_; }

private:
	void initialize_entity(World &world, Entity &entity);
	const ResourceIndex *index_ = nullptr;
	std::unordered_map<int32_t, std::string> model_names_;
	std::vector<std::pair<std::string, std::shared_ptr<const grm::File>>> models_;
	std::vector<FacialSlot> slots_;
	uint32_t display_frame_ = 0;
};

} // namespace opennova::world
