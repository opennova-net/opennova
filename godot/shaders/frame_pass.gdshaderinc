// Exact camera-device signatures for the engine's frame passes. These are
// device contracts, not user quality switches: the game camera is the beauty
// pass and FrameFx's shared-world camera is the isolated Q3 pass (the
// witnessed glow/envmap-duplicate queue, docs/render/render-order-re.md).
// The beauty pass also admits the first-person viewmodel layer (bit 11): the
// gun draws inside it through viewmodel_pass.gdshaderinc. Q3 omits that
// and the otherwise-unused terrain-shadow receiver plumbing bit, so it admits
// the same live world/water population without opening the viewmodel, a
// caster, or a slot-capture layer.
const uint NOVA_BEAUTY_CAMERA_MASK = 101377u; // 0x00018C01
const uint NOVA_Q3_CAMERA_MASK = 66561u;      // 0x00010401

// Every signature test carries IN_SHADOW_PASS: Godot's light-shadow passes
// leave the scene data's camera_visible_layers UNSET (stack garbage that can
// equal any signature, including a value left by the Q3 or a slot-capture
// render), and a signature branch taken inside a shadow map discards or
// rewrites casters. A shadow pass is never a frame-camera signature.
bool is_q3_pass(uint camera_visible_layers,
		bool in_shadow_pass) {
	return !in_shadow_pass &&
			camera_visible_layers == NOVA_Q3_CAMERA_MASK;
}
