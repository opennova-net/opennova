#include "authoring/preview_frame_effects.h"

#include <godot_cpp/classes/color_rect.hpp>
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/core/object.hpp>

#include <runtime/renderer/frame_fx_effects.h>

#include "particle/effect_distortion_drawer.h"
#include "particle/particle_renderer.h"
#include "render/frame_fx.h"

namespace godot {

namespace {

constexpr const char *kSunVeilShader = "res://shaders/sun_veil_overlay.gdshader";

} // namespace

PreviewFrameEffects::PreviewFrameEffects(Node3D &root, SubViewport &viewport, bool veil_drawn) {
	FrameFx *frame_fx = memnew(FrameFx);
	frame_fx->set_name("FrameFx");
	root.add_child(frame_fx);
	frame_fx_id_ = frame_fx->get_instance_id();
	if (!veil_drawn) return;
	ColorRect *veil = memnew(ColorRect);
	veil->set_name("SunVeil");
	veil->set_color(Color(1.0f, 1.0f, 1.0f, 1.0f));
	veil->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	veil->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
	const Ref<Shader> shader = ResourceLoader::get_singleton()->load(kSunVeilShader);
	if (shader.is_valid()) {
		Ref<ShaderMaterial> material;
		material.instantiate();
		material->set_shader(shader);
		veil->set_material(material);
	} else {
		veil->set_visible(false);
	}
	viewport.add_child(veil);
}

FrameFx *PreviewFrameEffects::frame_fx() const {
	return frame_fx_id_ ? Object::cast_to<FrameFx>(ObjectDB::get_instance(ObjectID(frame_fx_id_))) : nullptr;
}

void PreviewFrameEffects::present(ParticleRenderer *distortion) {
	FrameFx *frame_fx = this->frame_fx();
	if (frame_fx == nullptr) return;
	// GameWorld's legs in their order: the Q3 frame (sync_framefx_frame), then the screen effects' plan
	// (plan_screen_effects_frame) over the default view facts, the effects device drawing the row's distortion sets.
	frame_fx->advance_frame();
	frame_fx->set_view_effects(opennova::renderer::FrameFxViewInputs());
	std::shared_ptr<FrameFxDistortionDrawer> drawer;
	if (distortion != nullptr) drawer = distortion->distortion_drawer();
	frame_fx->set_distortion_drawer(drawer);
	frame_fx->advance_screen_effects();
}

} // namespace godot
