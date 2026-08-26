// Exact camera-device signatures for the engine's frame passes. These are
// device contracts, not user quality switches: the game camera is the beauty
// pass and FrameFx's shared-world camera is the isolated Q3 pass (the
// witnessed glow/envmap-duplicate queue, docs/render/render-order-re.md).
// Q3 omits only the otherwise-unused terrain-shadow receiver plumbing bit, so
// it admits the same live world/water population without opening a hidden
// viewmodel, caster, or slot-capture layer.
const uint NOVA_BEAUTY_CAMERA_MASK = 99329u; // 0x00018401
const uint NOVA_Q3_CAMERA_MASK = 66561u;     // 0x00010401

// Every signature test carries IN_SHADOW_PASS: Godot's light-shadow passes
// leave the scene data's camera_visible_layers UNSET (stack garbage that can
// equal any signature, including a value left by the Q3 or a slot-capture
// render), and a signature branch taken inside a shadow map discards or
// rewrites casters. A shadow pass is never a frame-camera signature.
bool nova_is_q3_pass(uint camera_visible_layers,
		bool in_shadow_pass) {
	return !in_shadow_pass &&
			camera_visible_layers == NOVA_Q3_CAMERA_MASK;
}
