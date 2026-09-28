// The retail skinned effects' vertex program: _BaseInc.fx
// CalcSkinWorldPosAndNormal as every skinned vertex shader compiles it
// (NumBones 4), run over the model's bone palette instead of Godot's skinning
// so the lit effects can light their first bone's frame (below).
//
// The palette is one row per skeleton bone, three RGBA32F texels holding the
// rows of the 3x4 matrix that carries a bind-space vertex into the model's
// skeleton space: the bone's posed global transform times its skin bind.
// ObjectModel publishes it whenever its Skeleton3D settles a pose. Retail's
// SkinWorldMatrixArray holds the same matrices through the entity's world
// matrix, which MODEL_MATRIX applies here; a vertex's index bytes name its
// strip's palette entries, the parts the mesh already carries
// (renderer::prepare_model_mesh).
// [orig: CRenderBatchQueue_FlushBatches @ 0x5DA170 (entry k is the matrix of
// the strip's bone-table entry k), the SetMatrixArray @ 0x5DA1B2]
uniform sampler2D u_skin_palette : filter_nearest, repeat_disable;
// Unbound (a model without a skeleton, the swatch spheres): the bind vertex.
uniform bool u_skin_palette_bound = false;

// A light fallback byte naming no table entry (the mesh's CUSTOM0 channel).
const uint OBJ_SKIN_NO_ENTRY = 255u;

struct ObjSkinVertex {
	vec3 position;
	// CalcSkinWorldPosAndNormal's normal: blended and normalized with
	// skinnormal (SkBasic, SkGlass), else the first entry's alone. The
	// Gouraud hemisphere of the lit effects reads it.
	vec3 geometric_normal;
	// The frame the effect lights: the blended normal for SkBasic and
	// SkGlass; for the lit effects the undeformed normal, tangent and
	// binormal through the light entry (obj_skin_vertex).
	vec3 normal;
	vec3 tangent;
	vec3 binormal;
	// The vertex carried rigidly by the light entry: where the lit effects
	// measure their point lights from.
	vec3 light_origin;
	// The light entry's rotation: the frame the lit effects take their light,
	// hemisphere and eye vectors into.
	mat3 light_frame;
};

void obj_skin_rows(uint bone, out vec4 r0, out vec4 r1, out vec4 r2) {
	int row = min(int(bone), textureSize(u_skin_palette, 0).y - 1);
	r0 = texelFetch(u_skin_palette, ivec2(0, row), 0);
	r1 = texelFetch(u_skin_palette, ivec2(1, row), 0);
	r2 = texelFetch(u_skin_palette, ivec2(2, row), 0);
}

vec3 obj_skin_point(vec4 r0, vec4 r1, vec4 r2, vec3 p) {
	vec4 p1 = vec4(p, 1.0);
	return vec3(dot(r0, p1), dot(r1, p1), dot(r2, p1));
}

vec3 obj_skin_vector(vec4 r0, vec4 r1, vec4 r2, vec3 v) {
	return vec3(dot(r0.xyz, v), dot(r1.xyz, v), dot(r2.xyz, v));
}

// A palette entry D3DXMatrixInverse refuses: a zero determinant (the right
// hand's zero-scale row, world §14.1.5) or one whose reciprocal is not
// finite. The entries are affine, so the 3x3 decides.
bool obj_skin_singular(vec4 r0, vec4 r1, vec4 r2) {
	float det = dot(r0.xyz, cross(r1.xyz, r2.xyz));
	float reciprocal = 1.0 / det;
	return det == 0.0 || isinf(reciprocal) || isnan(reciprocal);
}

// One step back through the strip's bone table: the entry becomes the light
// entry when its matrix inverts and none nearer did.
void obj_skin_light_candidate(uint entry, inout bool found,
		inout vec4 l0, inout vec4 l1, inout vec4 l2) {
	if (found || entry == OBJ_SKIN_NO_ENTRY) {
		return;
	}
	vec4 c0; vec4 c1; vec4 c2;
	obj_skin_rows(entry, c0, c1, c2);
	if (!obj_skin_singular(c0, c1, c2)) {
		l0 = c0;
		l1 = c1;
		l2 = c2;
		found = true;
	}
}

// NumBones 4: w0..w2 on index bytes 0..2 exactly as stored, and lastweight
// = 1 - (w0 + w1 + w2) on byte 3, never renormalized (a stored sum past 1
// gives byte 3 a negative share, as retail's does). With skinnormal the
// normal blends with the same weights and normalizes (SkBasic, SkGlass);
// without it (every lit bump effect) the normal is the first entry's alone
// and not normalized, and the effect lights In.Norm, In.Tangent and
// In.Binormal in its light entry's frame: the first entry's, whose inverse
// SkinModelLightArray[indexvector.x] carries, unless that matrix has none.
// Then the fill's inverse buffer still holds the last inverse it made, the
// nearest earlier table entry's that inverted (`light_fallbacks`, the parts
// before the first entry, nearest first; prepare_model_mesh). Before the
// table's first entry the buffer holds stale stack, read here as the
// identity. Retail's fill walks the whole table; the mesh records at most
// the four nearest earlier entries per vertex (the CUSTOM0 bytes), so a
// first entry whose four nearest predecessors all collapse reads the
// identity here too, where retail would light through the next earlier
// entry that inverted (the port's stated choice,
// docs/threedi/3di-gp-format-re.md). The Gouraud hemisphere reads the
// first entry's own normal,
// which a collapsed entry zeroes: a level hemisphere. Retail's in-shader
// transpose of a collapsed entry zeroes the phong light, eye and bumped
// hemisphere vectors too, and normalizes them to no defined value; the
// port lights those in the light entry's frame as well.
// [orig: _BaseInc.fx CalcSkinWorldPosAndNormal; _vsSkDfT.fx
// vsTanSkinDot3DirPS; _vsSkPhT.fx vsTanSkinPhongDir;
// CRenderBatchQueue_FlushBatches @ 0x5DA4F6..0x5DA5CE, @ 0x5DA950..0x5DA9A1]
ObjSkinVertex obj_skin_vertex(vec3 position, vec3 normal, vec3 tangent, vec3 binormal,
		uvec4 bones, vec4 weights, vec4 light_fallbacks) {
	ObjSkinVertex skin;
	skin.position = position;
	skin.geometric_normal = normal;
	skin.normal = normal;
	skin.tangent = tangent;
	skin.binormal = binormal;
	skin.light_origin = position;
	skin.light_frame = mat3(1.0);
	if (!u_skin_palette_bound) {
		return skin;
	}
	vec4 a0; vec4 a1; vec4 a2;
	vec4 b0; vec4 b1; vec4 b2;
	vec4 c0; vec4 c1; vec4 c2;
	vec4 d0; vec4 d1; vec4 d2;
	obj_skin_rows(bones.x, a0, a1, a2);
	obj_skin_rows(bones.y, b0, b1, b2);
	obj_skin_rows(bones.z, c0, c1, c2);
	obj_skin_rows(bones.w, d0, d1, d2);
	float last = 1.0 - (weights.x + weights.y + weights.z);
	skin.position = obj_skin_point(a0, a1, a2, position) * weights.x +
			obj_skin_point(b0, b1, b2, position) * weights.y +
			obj_skin_point(c0, c1, c2, position) * weights.z +
			obj_skin_point(d0, d1, d2, position) * last;
	#ifdef OBJ_SKIN_NORMAL_BLEND
	skin.normal = obj_unit_or_zero(obj_skin_vector(a0, a1, a2, normal) * weights.x +
			obj_skin_vector(b0, b1, b2, normal) * weights.y +
			obj_skin_vector(c0, c1, c2, normal) * weights.z +
			obj_skin_vector(d0, d1, d2, normal) * last);
	skin.geometric_normal = skin.normal;
	skin.light_origin = skin.position;
	#else
	skin.geometric_normal = obj_skin_vector(a0, a1, a2, normal);
	vec4 l0 = a0; vec4 l1 = a1; vec4 l2 = a2;
	if (obj_skin_singular(a0, a1, a2)) {
		l0 = vec4(1.0, 0.0, 0.0, 0.0);
		l1 = vec4(0.0, 1.0, 0.0, 0.0);
		l2 = vec4(0.0, 0.0, 1.0, 0.0);
		uvec4 chain = uvec4(round(clamp(light_fallbacks, 0.0, 1.0) * 255.0));
		bool found = false;
		obj_skin_light_candidate(chain.x, found, l0, l1, l2);
		obj_skin_light_candidate(chain.y, found, l0, l1, l2);
		obj_skin_light_candidate(chain.z, found, l0, l1, l2);
		obj_skin_light_candidate(chain.w, found, l0, l1, l2);
	}
	skin.normal = obj_skin_vector(l0, l1, l2, normal);
	skin.tangent = obj_skin_vector(l0, l1, l2, tangent);
	skin.binormal = obj_skin_vector(l0, l1, l2, binormal);
	skin.light_origin = obj_skin_point(l0, l1, l2, position);
	skin.light_frame = mat3(vec3(l0.x, l1.x, l2.x), vec3(l0.y, l1.y, l2.y),
			vec3(l0.z, l1.z, l2.z));
	#endif
	return skin;
}
