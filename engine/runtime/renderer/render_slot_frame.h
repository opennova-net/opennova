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
#include <functional>
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
	// The caster first, then the models linked capture-with it.
	std::vector<uint64_t> casters;
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
// `capture_links` holds (child, parent) for every model linked capture-with
// another caster. `terrain_height(x, z)` takes presentation planar
// coordinates; null keeps the anchor under the march start.
void slot_frame(const SlotFrameInput &input, const std::vector<SlotAssignment> &assignments,
		const SlotFrameCasterLookup &caster_for,
		const std::vector<std::pair<uint64_t, uint64_t>> &capture_links,
		const SlotFrameLightQuery &lights_for,
		const std::function<float(float, float)> &terrain_height, SlotFrame &out);

}  // namespace opennova::renderer
