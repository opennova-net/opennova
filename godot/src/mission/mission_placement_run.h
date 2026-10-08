#pragma once

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/transform3d.hpp>

#include <cstdint>
#include <vector>

#include "mission/mission_object_placer.h"
#include "mission/mission_placement_stats.h"

namespace godot {

// A placement run a unit at a time (ADR 0046 S14, decision D5): the OpenNova Editor's mission device
// places a mission over the Shell's frames within its budget, where the game places it whole
// (MissionObjectPlacer::place_rows, which is begin_place_rows and every step: the same walk in the
// same order, so the game's own path is unchanged). The units, in order: the rows bucketed by
// graphic and reflection policy and split static or animated, kBucketRowsPerUnit rows a unit; one
// unit per static group (a graphic under one policy: its batches harvested, its populations emitted
// over every bin); the animated models, kAnimatedPerUnit a unit; the finish (the census). A run
// begun on the placer after this one cancels it: its next step does nothing and it is done, its
// census never made (the editor's device drops a run when a newer rebuild begins, V6's rules).
//
// The unit sizes come from the placer measured on the largest shipped mission (ASH_I1gA.bms, 2,193
// entities, 398 animated, 1,037 statics over 133 graphics; headless, the template_debug library,
// 2026-10-01): cold, place() took 1,882 ms (bucket 52 ms, static batches 753 ms, animated models
// 1,073 ms); with every cache warm 458 ms (bucket 9 ms, static batches 65 ms, animated models 379
// ms). Stepped as this run with eight models a unit (242 units), the longest unit warm was a
// bucket's 1.8 ms, a static group's 2.6 ms and a models unit's 12.9 ms (16.8 ms on ASH_I1eA.bms:
// over a frame's 16 ms), so the models go four a unit (about 6 to 8 ms warm); cold, the longest
// were 20 ms, 30 ms (65 ms on ASH_I1eA) and 81 ms, a graphic's first load in each: the editor's
// device loads and warms every graphic in units of its own before it begins the run, so the run it
// steps is the warm one.
class MissionPlacementRun : public RefCounted {
	GDCLASS(MissionPlacementRun, RefCounted)

public:
	enum Step {
		STEP_MORE = 0,
		STEP_DONE = 1,
	};
	static constexpr int kBucketRowsPerUnit = 512;
	static constexpr int kAnimatedPerUnit = 4;

	// The next unit run: STEP_MORE while units are left, STEP_DONE at the last (the census made) or
	// once the run is done or cancelled.
	Step step();
	bool is_done() const { return done_; }
	// Cancelled by a run begun after it on the same placer: done, with no census.
	bool is_cancelled() const { return cancelled_; }
	// The units planned so far (the static groups are counted once the bucketing ends), how many
	// ran, and what the next one does ("bucket", "statics", "models", "finish"; "" when done).
	int get_step_count() const;
	int get_steps_done() const { return done_units_; }
	String get_step_label() const;
	// The census, once done (null before, and for a cancelled run).
	Ref<MissionPlacementStats> get_stats() const { return done_ && !cancelled_ ? stats_ : Ref<MissionPlacementStats>(); }
	uint64_t get_generation() const { return generation_; }

protected:
	static void _bind_methods();

private:
	friend class MissionObjectPlacer;

	// What one unit does.
	enum class Unit : uint8_t { Bucket, Statics, Models, Finish };
	// A graphic's instances under one reflection policy, as the bucketing gathers them: the parallel
	// per-slot arrays (shadow eligibility, identity, effect sources) share one slot order per group so
	// destruction can carve every matching draw by the same BMS index.
	struct StaticGroup {
		String graphic;
		bool mirror_reflected = false;
		Vector<Transform3D> xforms;
		Vector<bool> shadow_slots;
		Vector<int> bms_ids;
		Vector<int> item_ids;
		Vector<int> kinds;
		Vector<int> entity_indices;
		Vector<int> teams;
		Vector<uint32_t> entity_attribs;
		Vector<uint32_t> attrib2_values;
		Array effect_sources;
	};

	Unit next_unit_() const;
	int bucket_units_() const;
	int animated_units_() const;

	Ref<MissionObjectPlacer> placer_;
	uint64_t generation_ = 0;
	bool done_ = false;
	bool cancelled_ = false;
	bool empty_ = false; // nothing to place (no parent, no root): done at once with an empty census
	int done_units_ = 0;
	// The input.
	std::vector<MissionObjectPlacer::PlacementRow> rows;
	Callable progress;
	Array skip_kinds;
	// The nodes it places under, valid within a step; between steps they are known by their IDs (a
	// script holding the run may free them): a step finding the container gone cancels the run, one
	// finding the populations' holder gone mints another.
	Node3D *container = nullptr;
	Node3D *populations = nullptr;
	ObjectID container_id;
	ObjectID populations_id;
	// The bucketing, so far.
	size_t next_row = 0;
	HashMap<String, StaticGroup> static_groups;
	Vector<String> static_order;
	Array animated;
	int markers = 0;
	int unresolved = 0;
	// The static groups and the animated models placed so far.
	int next_group = 0;
	int next_animated = 0;
	int placed = 0;
	int batched = 0;
	int graphics = 0;
	int batch_count = 0;
	int binned_batch_count = 0;
	int global_batch_count = 0;
	int lod_population_count = 0;
	int shadow_batch_count = 0;
	HashMap<uint64_t, bool> occupied_static_bins;
	Vector<String> resolved_graphics;
	HashMap<String, int> profile_rows;
	int animated_count = 0;
	int authored_occluder_models = 0;
	// The stages' spans, microseconds, summed over their units.
	uint64_t bucket_usec = 0;
	uint64_t static_usec = 0;
	uint64_t animated_usec = 0;
	Ref<MissionPlacementStats> stats_;
};

} // namespace godot

VARIANT_ENUM_CAST(godot::MissionPlacementRun::Step);
