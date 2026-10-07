#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace opennova::editor {

// The kinds of viewport (ADR 0046 S13 V5; CONTEXT.md "Viewport") and where each is drawn, as the
// session's view names them: what the Preview window follows (DocumentsView::previews) is kept per
// kind. The enums and their wire tokens alone, so a reader of the view holds them without the
// preview layer; what a kind is (the document types it shows, what makes one) is the kinds' table,
// preview/viewport_kinds. A kind is one value here and one row in each of three tables, none of
// which builds without it: preview/viewport_kinds.cpp (what it is to the session),
// ui/viewport_views.cpp (the view a window draws it with) and godot/src/authoring/viewport_devices.cpp
// (the Shell's device for it).
enum class ViewportKind : uint8_t {
	Menu, // a menu's screen as the game draws it (the Shell's MenuFrame)
	Model, // a model, or a clip or an animation table played on its rig's model (ObjectModel)
	// A text document's text, edited in place (S13 V10, the script device: the Shell's CodeEdit, a
	// Control placed in the rect the tab reserves, which owns the input there)
	Script,
	// A mission as the game draws it (S14: its terrain, environment and entities, the Shell's own
	// Terrain, MissionEnvironment and MissionObjectPlacer), its marks drawn over it by its canvas
	Mission,
	// A texture as the game reads it (S18: its texels at a zoom, through its channels, at a mip level;
	// the Shell's texture device), the Document tab's view of a texture, and the Preview window's of a
	// texture Files selects
	Texture,
	// A particle effect as the game draws it (ADR 0046 DI-14: the effect a particle file defines,
	// spawned by the engine's effect scene and played on the preview clock; the Shell's particle
	// renderer), the Preview window's while a particle file is active
	Effect,
	// A HUD layout's HUD as the game draws it (the deep-integration plan's DI-20: hudpos.def through the
	// runtime's HudOverlay at a screen size, for a player whose state its options choose; the Shell's HUD
	// device), the Preview window's beside the layout's text
	Hud,
	// A definition table's selected record as the game draws the thing it defines (the deep-integration
	// plan's DI-21: an item's model in its state, its effects and its death's sounds, a person posed as it
	// spawns, a weapon's model, an ammo's round), the Preview window's beside the table
	Definition,
	// An environment's sky as the game draws it (the deep-integration plan's DI-19b: its time of day on the
	// game's mission clock, its weather as a script sets it, over the terrain of a mission that runs on it;
	// the Shell's MissionEnvironment, Weather, SkyDome, Celestial, Water, Terrain and drops), the Document
	// tab's main view
	Environment,
	// A terrain drawn on its own (the deep-integration plan's DI-30b: the runtime's terrain, water, foliage and
	// environment under a mission that runs on it, or the engine's own; DI-29's overlays; DI-07's ground under the
	// pointer), the Document tab's main view
	Terrain,
	kCount,
};

inline constexpr size_t kViewportKindCount = static_cast<size_t>(ViewportKind::kCount);

// Where a viewport of the kind is drawn: in the Preview window beside the Document tab (the
// menu's and the model's), or as the Document tab's main view (ui/document_views' MainViewport
// role: a text document's script device, S13 V10; the mission's 3D view, S14, with ImGui
// overlays, the document's outline and the Inspector beside it). A document type is shown by one
// Main-role kind at most and fed by one Preview-role kind at most: a mission's 3D view and its map
// are one Main and one Preview.
enum class ViewportRole : uint8_t { Preview, Main };

// A kind's token on the wire ("menu", "model", "script", "mission", "texture", "effect", "hud", "definition",
// "environment"; "" past
// the last kind), and
// the kind a token names (false for none).
const char *viewport_kind_token(ViewportKind kind);
bool viewport_kind_from_token(const std::string &token, ViewportKind &out);

} // namespace opennova::editor
