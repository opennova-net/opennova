// The render-slot pass's per-slot frame: for every slot the plan assigned
// (render_slot_shadow.h RenderSlotPlan::assign), what the frame captures and
// what it drapes, composed from the planner's leaf laws. Retail runs the
// three stages at three sites: the light pick and march in the entity update
// [orig: RenderSlot_UpdateEntityLight @ 0x5d6a30, called from
// Entity_UpdateAllEntities @ 0x4c25fa], the silhouette capture in the shadow
// pass [orig: RenderSlot_RenderEntityAndChildren @ 0x5d7690, called from
// Render_ShadowPass @ 0x5d7c95] and the drapes in the terrain render
// [orig: RenderSlot_DrawAllDrapes @ 0x5d6e20, called from
// Terrain_RenderMainSectorPass @ 0x610c47 -> RenderSlot_DrawSilhouetteDrape
// @ 0x5d5ca0]; this function composes each stage's per-slot outcome once per
// display frame (the pick's cadence, render-lighting-re.md's accepted
// divergence), each stage's own rules as retail orders them. The embedder
// marshals the casters, their capture-with links,
// the point lights a slot may pick and the terrain height, and keeps the
// device half: the capture targets and their draws, the capture eye
// (docs/render/render-lighting-re.md D-RLIT-10), the drape projection and
// the uniform writes.
#pragma once

#include <runtime/renderer/render_slot_shadow.h>

#include <array>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace opennova::renderer {

// One caster's facts for this frame, presentation axes (x, y up, z = -north),
// world units.
struct SlotFrameCaster {
	// The plan's inputs: the entity bound (entity+0) sizing the lod, the
	// patch and the light query, and the person, interior and local-player
	// classes.
	SlotCandidateState state{};
	// The model sphere (gpm[5]) the capture extent and the depth clip size
	// from [orig: RenderSlot_RenderEntityAndChildren @ 0x5d7835 reads the
	// model's +0x14, not entity+0].
	float capture_radius = 1.0f;
	// The entity origin (entity+4..+0xC): every slot quantity keys on it —
	// the light query box, the pick distance, the capture's view origin, the
	// drape distance and projection — never on the render bounds, which move
	// with part animation and RLOD switches [orig: the query box
	// @ 0x5d6af1..0x5d6b39, the pick distance origin @ 0x5d6bbd..0x5d6bc9, the
	// entity rendered at the view origin by Entity_RenderWithLODCallback
	// @ 0x5d6fc4..0x5d6fd9].
	std::array<float, 3> position{};
	// slot_march_start_offset's result for this entity.
	std::array<float, 3> march_offset{};
};

// A draw's visibility as the slot pass reads it: the sim's present intent
// (retail's entity Flags & 1, the hidden / carried bit), the draw's own state
// (which also carries an attached draw's direct hide), and whether a
// render-only claim hides it this frame (the occlusion collect's, the
// sub-pixel floor's). The slot pass reads no render gate: the sort gates only
// Flags & 1 [orig: RenderSlot_SortAndAssign @ 0x5d657d], the drape walk skips
// the same bit [orig: RenderSlot_DrawAllDrapes @ 0x5d6e6a] and the capture
// draws its caster with no visibility test [orig: RenderSlot_RenderEntityAndChildren
// @ 0x5d7690], so a claimed draw counts by its present intent. `scene_hidden`
// is the embedder's own hiding of the draw's parent (an editor's hidden layer,
// a hidden world), a state of the device, not of retail. `local_view_suppressed`
// marks a row whose Flags & 1 is clear that the local view keeps off the camera
// (world::PF_LOCAL_VIEW_SUPPRESSED): a camera-mode-0 render-callback verdict,
// which the slot's capture meets again through that same callback, since the
// shadow pass runs under the player's camera mode — the `tank` class draws
// its virtual-display model for the local claimant, and the UseGun mount
// nothing [orig: Render_DestroyedSections @ 0x449ef0, the gate @ 0x449f12..0x449f27,
// the swap @ 0x449f29..0x449f45; Entity_RenderVehicleModel @ 0x4407f6..0x44084c;
// Render_ShadowPass @ 0x5d7b70 writes no g_CameraMode]. The sort never reads it
// [orig: RenderSlot_SortAndAssign @ 0x5d657d].
struct SlotCasterVisibility {
	bool present_visible = true;
	bool draw_visible = true;
	bool render_claimed = false;
	bool scene_hidden = false;
	bool local_view_suppressed = false;
};

// Whether a draw counts in the slot pass: its own state, the render claims
// lifted (the parent's scene state aside).
constexpr bool slot_draw_shown(const SlotCasterVisibility &visibility) noexcept {
	return visibility.render_claimed ? visibility.present_visible : visibility.draw_visible;
}

// Whether the slot sort counts the draw: its present intent alone, the render
// claims and the local view's verdict lifted [orig: RenderSlot_SortAndAssign
// @ 0x5d657d tests Flags & 1 alone].
constexpr bool slot_sort_shown(const SlotCasterVisibility &visibility) noexcept {
	return visibility.local_view_suppressed || slot_draw_shown(visibility);
}

// Whether a capture draws the draw's own geometry outside a child walk: shown,
// and no local-view verdict swapping it out (its stand-in, the virtual display,
// rides the capture as a capture-with child).
constexpr bool slot_capture_draws(const SlotCasterVisibility &visibility) noexcept {
	return !visibility.local_view_suppressed && slot_draw_shown(visibility);
}

// The lighting an entity's slot reads: the contained flag and the interior
// light group its first blink hit names (the building's BMS id and the
// volume's section) [orig: RenderSlot_UpdateEntityLight @ 0x5d6adc..0x5d6aed
// (entity+0x1D0 zeroes the pick threshold), @ 0x5d6b40..0x5d6b77 (the group)].
struct SlotCasterLighting {
	bool interior = false;
	int interior_bms = 0;
	int interior_section = 0;
};

// One caster draw as the presenters stamp it from its entity (the present
// rows, the lighting feed, the placer's model radii): every fact the slot pass
// reads. A caster is a draw, not only an entity: a held weapon or a person
// overlay renders inside its entity's slot (capture_with) and the local
// player's avatar parts carry their own draws. `id` names the draw; links
// name other casters by theirs.
struct SlotCasterFacts {
	uint64_t id = 0;
	// The draw holds a slot record of its own: an entity the init registered (a
	// person, an ItemDef DynamicShadow item) or a draw linked into one. False
	// for a draw only a carrier's refNum walk renders, which retail draws
	// whatever slot it holds [orig: RenderSlot_RenderEntityAndChildren
	// @ 0x5d79a0..0x5d79c8 reads no slot of the member].
	bool slot_record = true;
	SlotCasterVisibility visibility{};
	// The ride facts (renderer::SlotRideBit) and the caster draw of the entity
	// this one stands on (entity+0x28 groundEntity), 0 for none or one not drawn.
	uint32_t ride_bits = 0;
	uint64_t ground_id = 0;
	// The seat facts (renderer::SlotSeatBit) and the caster draw whose capture's
	// pool-0 child walk draws this one (slot_seat_walk: the parentEntity, or
	// for a UseGun gunner its gun's groundEntity), 0 for none or one not drawn.
	uint32_t seat_bits = 0;
	uint64_t seat_walk_id = 0;
	// The caster whose submission renders this one inside its own: 0 for none.
	// A person's held weapon and overlays draw inside its world callback's
	// submit [orig: BoneCallback_org0_World @ 0x4e3940, the body @ 0x4e3c82, the
	// +0x2B0 gun test @ 0x4e3c97], the local claimant's virtual display inside
	// its hull's [orig: Render_DestroyedSections @ 0x449f29..0x449f45], so each
	// rides the capture that draws its owner [orig: RenderSlot_RenderEntityAndChildren
	// @ 0x5d78ef..0x5d79d6].
	uint64_t capture_with = 0;
	// Drawn under another caster in the scene (a model parented under it): it
	// renders with that caster and takes no slot of its own.
	bool parented_under_caster = false;
	bool is_person = false;
	// The model sphere (gpm[5]) and the entity bound (entity+0), world units;
	// a sphere of 0 is a draw the placer did not stamp, whose slot takes
	// `fallback_radius` (half its render-bounds diagonal, at least 0.5 u) for
	// both.
	float model_sphere_radius = 0.0f;
	float entity_bound_radius = 0.0f;
	float fallback_radius = 0.0f;
	// The entity origin and the march start's offset from it (present axes).
	std::array<float, 3> position{};
	std::array<float, 3> march_offset{};
	// The draw's own lighting and, for an attached draw, its owner entity's:
	// an attached draw renders inside its owner's submission and reads the
	// owner's lighting.
	SlotCasterLighting lighting{};
	bool has_light_owner = false;
	SlotCasterLighting owner_lighting{};
};

// The light query a slot's pick weighs around its caster: the entity origin,
// its bound and its interior group (the collector over entity+4 +- entity+0
// [orig: RenderSlot_UpdateEntityLight @ 0x5d6af1..0x5d6b39]).
struct SlotCasterLightQuery {
	std::array<float, 3> position{};
	float bound_radius = 0.0f;
	int interior_bms = 0;
	int interior_section = 0;
};

// The frame's draw links, each (child, parent): the draws rendered inside
// another caster's submission (SlotCasterFacts::capture_with), the pool-0
// child walk's riders and the carrier each rides (slot_seat_walk), and the
// refNum walk's riders and their carrier (slot_ride_walk_child).
struct SlotFrameLinks {
	std::vector<std::pair<uint64_t, uint64_t>> capture;
	std::vector<std::pair<uint64_t, uint64_t>> seat;
	std::vector<std::pair<uint64_t, uint64_t>> ride;
};

class SlotFrameCasters;
// Builds the frame's casters from their facts: the hidden rule (Flags & 1,
// the render claims and the local view's verdict lifted, the scene parent's
// state), the capture-with and scene-parent exclusions, the person's
// seat-parent exclusion and the pool-0 walk links (slot_seat_parented,
// slot_seat_walk), the vehicle-ride exclusion and the refNum-walk
// links (slot_rides_vehicle, slot_ride_walk_child), the person class, the
// local player's and its ridden vehicle's priority [orig: RenderSlot_SortAndAssign
// @ 0x5d669c..0x5d66a6 (cmp [ebp+16Ch]), the halving @ 0x5d6864;
// RenderSlot_RenderEntityAndChildren @ 0x5d7707..0x5d7734, the every-frame
// refresh], the contained flag (the owner's for an attached draw), the radii
// and their fallback. `local_parent_id` is the vehicle the local player rides
// (entity+0x16C), 0 for none. `out` keeps its capacity.
void slot_frame_casters(const std::vector<SlotCasterFacts> &facts, uint64_t local_id,
		uint64_t local_parent_id, SlotFrameCasters &out);

// The frame's casters: one SlotFrameCaster per fact in order, the frame's
// draw links and each caster's light query.
class SlotFrameCasters {
public:
	std::vector<uint64_t> ids;
	// The ids that hold a slot record (SlotCasterFacts::slot_record): the ones
	// the embedder registers with the plan.
	std::vector<uint64_t> slot_ids;
	std::vector<SlotFrameCaster> casters;
	std::vector<SlotCasterLightQuery> light_queries;
	SlotFrameLinks links;

	const SlotFrameCaster *find(uint64_t id) const;
	const SlotCasterLightQuery *light_query(uint64_t id) const;
	// The plan's state for an id; a caster gone this frame is hidden.
	SlotCandidateState state_for(uint64_t id) const;

private:
	friend void slot_frame_casters(const std::vector<SlotCasterFacts> &facts, uint64_t local_id,
			uint64_t local_parent_id, SlotFrameCasters &out);
	std::unordered_map<uint64_t, std::size_t> index_;
};

struct SlotFrameInput {
	// The frame-open slot direction as stored: slot_projection_direction's
	// raw clamped-negated sun.
	std::array<float, 3> sun_direction{0.0f, -1.0f, 0.0f};
	// The drape's sun and sky colours (g_EnvLightBlock, g_EnvSkyBlock), 0..1.
	std::array<float, 3> sun_rgb{};
	std::array<float, 3> sky_rgb{};
	std::array<float, 3> camera_position{};
	uint32_t frame = 0;
	int shadow_detail = 0;
	// The local player (its first-person drape gate) and its view state.
	uint64_t local_id = 0;
	bool local_first_person = false;
	bool local_prone = false;
	// Whether the terrain sector pass draws this frame: a closed one skips the
	// drape patches while the captures and the terms keep running.
	bool terrain_pass_drawn = true;
};

// A slot's silhouette capture.
struct SlotFrameCapture {
	int order = -1;  // 0..11, the silhouette RT
	int size = 0;    // slot_texture_size
	// The capture renders this frame (the refresh cadence or a dirty order);
	// an unarmed order keeps its previous image.
	bool armed = false;
	// The capture view along the slot direction (slot_capture_view_axes),
	// centred on the caster's origin, its ortho half extent from the model
	// sphere (silhouette_half_extent).
	SlotCaptureViewAxes axes{};
	std::array<float, 3> center{};
	float half_extent = 0.0f;
	// The caster first, then the draws its submission carries (its capture-with
	// children) and those its pool-0 walk riders' submissions carry, each by its
	// own visibility; then from `walk_begin` on the walked riders: the pool-0
	// walk's (slot_seat_walk), then the refNum walk's. Both walks draw each
	// rider through the rider's own render callback with no visibility test
	// [orig: RenderSlot_RenderEntityAndChildren — the pool-0 draws @ 0x5d7938 and
	// @ 0x5d7963, the refNum draw @ 0x5d79c8; Entity_RenderWithLODCallback
	// @ 0x5d6ef0 tests no Flags], so neither the rider's present intent
	// (Flags & 1) nor a render claim hides it: the device draws its geometry
	// whatever its visibility. The refNum walk runs under camera mode 1
	// [orig: RenderSlot_RenderEntityAndChildren — g_CameraMode = 1 @ 0x5d7978,
	// restored @ 0x5d79dc], lifting the first-person culls that test camera
	// mode 0 [orig: Entity_RenderVehicleModel @ 0x440808; Render_DestroyedSections
	// @ 0x449f20]; the pool-0 walk keeps the player's mode, whose verdicts no
	// person takes (the shadow pass runs with no camera-tracked entity, so a
	// person's world callback draws the local body too [orig: Render_TerrainScene
	// @ 0x610cab..0x610cbd; BoneCallback_org0_World @ 0x4e3ab3]). The caster's own
	// geometry draws by slot_capture_draws.
	std::vector<uint64_t> casters;
	std::size_t walk_begin = 0;
};

// A slot's terrain drape.
struct SlotFrameDrape {
	// False when the slot drapes nothing this frame: the local player's
	// first-person gate or the 80 u cull. Its term is then zero.
	bool drawn = false;
	// The fixed-function material ambient per channel.
	std::array<float, 3> ambient{};
	// The attached-light leg's light 4 (range 0 = the sun leg).
	SlotDrapeLight light{};
	// The grazing-rescaled lod and the patch around the marched anchor
	// (patch_drawn false while the terrain pass is closed).
	int lod = 0;
	bool patch_drawn = false;
	SlotPatch patch{};
	SlotDepthClip clip{};
};

struct SlotFrameSlot {
	uint64_t id = 0;
	SlotFrameCapture capture{};
	SlotFrameDrape drape{};
};

struct SlotFrame {
	int bound = 0;     // slots holding a drape patch, not excluded
	int captures = 0;  // slots that drape their silhouette
	// The slots that own a silhouette RT, in the plan's order.
	std::vector<SlotFrameSlot> slots;
};

// The caster an assigned id names (null: gone this frame).
using SlotFrameCasterLookup = std::function<const SlotFrameCaster *(uint64_t)>;
// The point lights a slot's pick weighs (light_scene's query around the
// caster); a null query picks the sun for every slot.
using SlotFrameLightQuery = std::function<const std::vector<SlotPointLight> &(uint64_t)>;

// Fills `out` (its vectors keep their capacity) for this frame's assignments.
// `links` holds the frame's draw links (SlotFrameCasters::links).
// `terrain_height(x, z)` takes presentation planar coordinates; null keeps
// the anchor under the march start.
void slot_frame(const SlotFrameInput &input, const std::vector<SlotAssignment> &assignments,
		const SlotFrameCasterLookup &caster_for, const SlotFrameLinks &links,
		const SlotFrameLightQuery &lights_for,
		const std::function<float(float, float)> &terrain_height, SlotFrame &out);

}  // namespace opennova::renderer
