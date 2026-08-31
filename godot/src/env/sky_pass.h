#pragma once

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/node_path.hpp>

#include <runtime/environment/sky_frame.h>

namespace godot {

class MissionEnvironment;
class Weather;

// The sky-pass driver — the device leg over the engine's
// env::build_sky_frame (engine/runtime/environment), pushing the per-frame
// witnessed dome state (TOD palette, sun/light directions, skyfog, scroll,
// cloud textures) into the scene Sky's merged sky.gdshader material, which
// MissionEnvironment owns (ADR 0043 d3 amendment: the dome mesh retired into
// a real `shader_type sky` background — the shader evaluates the witnessed
// dome surface analytically, so this node keeps no mesh, no anchor, and no
// per-pass reanchor plumbing). The camera resolve here feeds only the cloud
// scroll's world x/z terms. Renamed from SkyDome with the mesh's retirement;
// ported from sky.gd (2026-08-10 de-scripting); RE record:
// docs/env/env-tod-re.md.
class SkyPass : public Node {
	GDCLASS(SkyPass, Node)

public:
	void set_environment_path(const NodePath &p_path);
	NodePath get_environment_path() const { return environment_path_; }
	void set_weather_path(const NodePath &p_path);
	NodePath get_weather_path() const { return weather_path_; }

	// Forwarder to the environment node's sky material (one call site for
	// diagnostics and tests).
	Ref<ShaderMaterial> get_sky_material();

	// One render-frame advance (the _process body) — the externally-callable
	// drive the test harness uses; the engine's virtual delegates here.
	void advance_frame(double p_delta);

	void _ready() override;
	void _process(double p_delta) override;

protected:
	static void _bind_methods();

private:
	MissionEnvironment *_env_node();
	Weather *_weather_node();
	void _update_cloud_textures(MissionEnvironment *p_env,
			const Ref<ShaderMaterial> &p_material);

	NodePath environment_path_;
	NodePath weather_path_;
	Ref<Texture2D> bound_cloud_tex1_;
	Ref<Texture2D> bound_cloud_tex2_;
	ObjectID env_node_id_;
	ObjectID weather_node_id_;
	ObjectID cached_cam_id_;
	// Standalone fallback (owners with no weather node): the engine-owned
	// private scroll core + 62 Hz credit.
	opennova::env::ScrollFallback fallback_scroll_;
};

} // namespace godot
