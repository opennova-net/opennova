#pragma once

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>

#include <memory>

#include "authoring/preview_frame_effects.h"

namespace godot {

class MissionEnvironment;

// The scene an orbit device draws in, the one setup the definition's, the effect's and the model's appliers share
// (ADR 0046): the view single-sampled, as the game's own view draws (the particle renderer's compositor passes bind
// its depth: under MSAA every particle went undrawn, DI-14's rule); the editor's preview background behind everything
// (authoring/preview_backdrop); a root holding the game's FrameFx for the view (its one terminal display decode, and
// the distortion row a class-7 particle draws on), the environment with no .env (it lights like the retail noon, the
// particle tints a mission's light gives), the camera with the game's horizontal field of view, made current, and
// with `grid` the metres' grid (named "Grid"). Every node is the viewport's child; the applier takes each into its
// own members and lays its own nodes under `root`.
struct OrbitStage {
	MeshInstance3D *backdrop = nullptr;
	Node3D *root = nullptr;
	std::unique_ptr<PreviewFrameEffects> frame_effects;
	MissionEnvironment *environment = nullptr;
	Camera3D *camera = nullptr;
	MeshInstance3D *grid = nullptr; // null without one
};
OrbitStage make_orbit_stage(SubViewport &viewport, bool grid);

} // namespace godot
