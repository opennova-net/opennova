// The GP writer (threedi_gp_write.h): parse()'s read order written back, the words the reader
// skips as BHD's model tool writes them. Each section cites the tool's writer; the layout and
// its dfbhd witnesses are threedi_gp.h and docs/threedi/3di-gp-format-re.md.

#include <formats/threedi_gp/threedi_gp_write.h>

#include <cstring>

#include <base/io/byte_writer.h>

namespace opennova::threedi_gp {
namespace {

using io::ByteWriter;

// The tag the tool writes over each primitive's +4, which the loader overwrites with the
// primitive's index offset [orig: GP_WriteRenderModel @ 0x4474D6, @ 0x447786 (df4oed)].
constexpr uint32_t kPrimitiveTag = 0x72646441u; // "Addr"

struct Writer {
	ByteWriter &w;
	std::string &error;

	bool fail(const std::string &what) {
		error = what;
		return false;
	}
	void i32x3(const std::array<int32_t, 3> &v) {
		for (int32_t x : v) w.write_i32(x);
	}
	void f32x3(const std::array<float, 3> &v) {
		for (float x : v) w.write_f32(x);
	}
	// A name field: a full one carries no terminator, as BHD's own CTRL names do
	// (VEHICLE_STEERING), and the reader stops at the field's end.
	bool name(const std::string &s, size_t width, const char *what) {
		if (s.size() > width) return fail(std::string(what) + " is longer than " + std::to_string(width) + " characters: " + s);
		w.write_fixed_string(s.c_str(), width);
		return true;
	}
	void generator(const Generator &g) {
		w.write_u8(g.style);
		w.write_u8(g.param);
		w.write_i16(g.rate);
		w.write_i16(g.start);
		w.write_i16(g.end);
	}

	// [orig: GP_WriteModel @ 0x448B77..0x448F88 (df4oed): the 0xEC header]
	bool header(const File &f) {
		const Header &h = f.header;
		const char kind = h.kind == Kind::Gpm ? 'M' : h.kind == Kind::Gps ? 'S' : h.kind == Kind::Gpp ? 'P' : '\0';
		if (kind == '\0') return fail("the model has no GP kind");
		if (h.revision > kMaxRevision) return fail("the revision is past 0x103");
		if (f.lods.empty() || f.lods.size() > static_cast<size_t>(kMaxLods)) return fail("the LOD count is outside 1..4");
		w.write_u8('G');
		w.write_u8('P');
		w.write_u8(static_cast<uint8_t>(kind));
		w.write_u8(kFormatVersion);
		w.write_u32(h.revision);
		if (!name(h.name, 16, "the model's name")) return false;
		w.write_u32(h.flags);
		w.write_i32(static_cast<int32_t>(f.lods.size()));
		for (int32_t t : h.thresholds) w.write_i32(t);
		w.write_zeros(16);
		for (uint32_t t : h.tags) w.write_u32(t);
		w.write_zeros(16);
		w.write_i32(h.radius);
		w.write_i32(h.radius_xy);
		w.write_i32(h.height);
		w.write_zeros(0x88 - 0x6C);
		w.write_i32(static_cast<int32_t>(f.vertices.size()));
		w.write_zeros(0xB0 - 0x8C);
		w.write_i32(static_cast<int32_t>(f.user_points.size()));
		w.write_zeros(0xBC - 0xB4);
		w.write_i32(static_cast<int32_t>(f.control_registers.size()));
		w.write_zeros(4);
		w.write_i32(static_cast<int32_t>(f.matrices.size()));
		w.write_zeros(0xD0 - 0xC8);
		w.write_i32(static_cast<int32_t>(f.occlusion.size()));
		w.write_zeros(kHeaderSize - 0xD4);
		return true;
	}

	// [orig: GP_WriteUserPoints @ 0x443310 (df4oed)]
	bool user_points(const std::vector<UserPoint> &points) {
		for (const UserPoint &p : points) {
			i32x3(p.position);
			i32x3(p.direction);
			w.write_i32(p.part);
			w.write_u8(p.type);
			w.write_zeros(3);
			if (!name(p.name, 16, "a user point's name")) return false;
		}
		return true;
	}

	// [orig: GP_WriteTextureTable @ 0x4485B0 (df4oed)]
	bool textures(const std::vector<TextureRow> &rows) {
		w.write_u32(static_cast<uint32_t>(rows.size()));
		for (const TextureRow &t : rows) {
			if (!name(t.name, 16, "a texture name") || !name(t.alpha, 16, "a texture's alpha name")) return false;
			w.write_zeros(4);
			w.write_i16(t.id);
			w.write_u16(t.flags);
			w.write_u16(t.width);
			w.write_u16(t.height);
			w.write_zeros(16);
		}
		return true;
	}

	// [orig: GP_WriteCollision @ 0x443980 (df4oed)]: the 0x88 header, then the vertices,
	// normals, faces, sections, translations, planes and volumes in one blob.
	void collision(Kind kind, const Collision &c) {
		const size_t blob = c.vertices.size() * 8 + c.normals.size() * 8 + c.faces.size() * 44 + c.sections.size() * 128 +
				c.translations.size() * 12 + c.planes.size() * 16 + c.volumes.size() * 96;
		// +0: 1 on a skinned model's block [orig: @ 0x443B77 (df4oed)]; no dfbhd reader found.
		w.write_i32(kind == Kind::Gpm ? 0 : 1);
		w.write_u32(static_cast<uint32_t>(blob));
		w.write_zeros(4);
		i32x3(c.radii);
		for (int32_t b : c.box) w.write_i32(b);
		const size_t counts[7] = {c.vertices.size(), c.normals.size(), c.faces.size(), c.sections.size(),
				c.translations.size(), c.planes.size(), c.volumes.size()};
		for (size_t n : counts) {
			w.write_i32(static_cast<int32_t>(n));
			w.write_zeros(4);
		}
		w.write_zeros(0x88 - 0x68);
		for (const CollisionVertex &v : c.vertices) {
			w.write_i16(v.x);
			w.write_i16(v.y);
			w.write_i16(v.z);
			w.write_i16(v.section);
		}
		for (const CollisionNormal &n : c.normals) {
			w.write_i16(n.x);
			w.write_i16(n.y);
			w.write_i16(n.z);
			w.write_i16(n.axis);
		}
		for (const CollisionFace &f : c.faces) {
			for (int16_t k : f.corners) w.write_i16(k);
			w.write_i16(f.normal);
			w.write_i32(f.plane);
			for (int32_t b : f.box) w.write_i32(b);
			w.write_u32(f.flags);
			w.write_u8(f.surface);
			w.write_zeros(3);
		}
		for (const CollisionSection &s : c.sections) {
			w.write_i32(s.flags);
			w.write_i32(s.vertex_count);
			w.write_zeros(4);
			w.write_i32(s.face_count);
			w.write_zeros(4);
			w.write_i32(s.normal_count);
			w.write_zeros(4);
			w.write_i32(s.volume_count);
			w.write_zeros(4);
			w.write_i32(s.parent);
			w.write_zeros(12);
			i32x3(s.offset);
			for (int32_t b : s.box) w.write_i32(b);
			i32x3(s.centre);
			w.write_i32(s.radius);
			w.write_i32(s.radius_xy);
			w.write_i32(s.height);
			w.write_zeros(16);
		}
		for (const auto &t : c.translations) i32x3(t);
		for (const CollisionPlane &p : c.planes) {
			i32x3(p.normal);
			w.write_i32(p.d);
		}
		for (const CollisionVolume &v : c.volumes) {
			w.write_i32(v.type);
			w.write_i32(v.flags);
			i32x3(v.centre);
			w.write_i32(v.radius);
			w.write_i32(v.radius_xy);
			w.write_i32(v.radius_xy); // the tool stores the xy radius twice [orig: @ 0x444D7D..0x444DA3 (df4oed)]
			i32x3(v.half);
			w.write_zeros(4);
			for (int32_t b : v.box) w.write_i32(b);
			w.write_i32(v.plane_count);
			w.write_zeros(20);
		}
	}

	// [orig: GP_WriteModel @ 0x4490EF..0x449238 (df4oed)]: GPP's 60-byte vertices and GPM's
	// 44-byte ones, each with the packed colour the loader drops (0xFF000000 from the tool).
	bool vertices(Kind kind, const std::vector<Vertex> &verts) {
		for (const Vertex &v : verts) {
			f32x3(v.position);
			if (kind == Kind::Gps) w.write_f32(v.blend);
			if (kind == Kind::Gpp) {
				f32x3(v.weights);
				for (uint8_t p : v.palette) {
					if (p > 15) return fail("a vertex's palette slot is past 15");
					w.write_u8(p);
				}
			}
			f32x3(v.normal);
			w.write_u32(0xFF000000u);
			w.write_f32(v.uv0[0]);
			w.write_f32(v.uv0[1]);
			w.write_f32(v.uv1[0]);
			w.write_f32(v.uv1[1]);
		}
		return true;
	}

	void primitive(bool global, const Primitive &p) {
		w.write_i32(p.material);
		w.write_u32(kPrimitiveTag);
		w.write_u16(static_cast<uint16_t>(p.indices.size()));
		w.write_u16(0); // the triangle count, 0 on disk
		w.write_i32(p.topology);
		w.write_i32(p.first_vertex);
		w.write_i32(p.vertex_word);
		w.write_bytes(p.palette.data(), p.palette.size());
		if (global) {
			w.write_zeros(7);
			w.write_u8(p.palette_length);
		}
		for (uint16_t i : p.indices) w.write_u16(i);
	}

	// [orig: GP_WriteMaterials, in GP_WriteRenderModel @ 0x4478B9..0x447D7D (df4oed)]
	bool material(const Material &m) {
		if (!name(m.name, 16, "a material's name")) return false;
		w.write_u32(m.attributes);
		w.write_i16(m.frame_rate);
		w.write_i8(m.frame_count);
		w.write_u8(m.frame_counter);
		w.write_zeros(16); // the region colour (runtime) and its triplets
		for (int8_t id : m.region_textures) w.write_i8(id);
		w.write_i8(m.alpha_texture);
		w.write_zeros(4); // the environment tint
		w.write_u8(m.env_pass);
		w.write_zeros(1);
		w.write_u8(m.shader);
		w.write_zeros(5);
		w.write_u8(m.colour_source);
		w.write_u8(m.alpha_source);
		w.write_u8(m.alpha);
		w.write_u8(m.alpha_ref);
		w.write_u8(m.blend);
		w.write_u8(m.paired);
		w.write_zeros(10);
		// +0x48, +0x4C: the tilings no reader takes, 1 from the tool [orig: @ 0x447AF3..0x447B16 (df4oed)].
		w.write_f32(1.0f);
		w.write_f32(1.0f);
		generator(m.u);
		generator(m.v);
		w.write_u8(m.rgb.style);
		w.write_u8(m.rgb.param);
		w.write_i16(m.rgb.rate);
		w.write_bytes(m.rgb.start.data(), 4);
		w.write_bytes(m.rgb.end.data(), 4);
		generator(m.alpha_gen);
		w.write_zeros(152 - 0x74);
		return true;
	}

	// A part's animation row [orig: GP_WriteRenderModel @ 0x447DC1..0x4481F5 (df4oed)]; a
	// spinner's first 12 track bytes are its three float rates.
	void part_animation(const PartAnimation &a) {
		w.write_u32(a.flags);
		w.write_u8(a.parent);
		w.write_u8(a.part);
		w.write_zeros(2);
		w.write_i32(a.frame);
		if (a.flags & kPanmSpinner) {
			for (float r : a.spin) w.write_f32(r);
			w.write_i16(a.tracks[1].start);
			w.write_i16(a.tracks[1].end);
		} else {
			generator(a.tracks[0]);
			generator(a.tracks[1]);
		}
		for (size_t k = 2; k < a.tracks.size(); ++k) generator(a.tracks[k]);
		w.write_zeros(92 - 60);
	}

	// [orig: GP_WriteRenderModel @ 0x446D90 (df4oed)]: the 0x88 header, then the blob: attach
	// points, parts, batches (a global table's header first), primitives, materials, part
	// animations.
	bool render_model(const RenderModel &m) {
		if (m.extra_count != 0 || m.record_count != 0)
			return fail("a render model holds extra primitives or records this writer does not carry");
		const bool global = (m.flags & kRenderGlobalBatch) != 0;
		const bool animated = (m.flags & kRenderPartAnimations) != 0;
		if (animated && m.part_animations.size() != m.parts.size())
			return fail("a render model's part animations are not one a part");
		std::vector<int32_t> part_batches(m.parts.size(), 0);
		size_t primitives = 0;
		int last = 0;
		for (const Batch &b : m.batches) {
			if (b.opaque_count < 0 || b.alpha_count < 0) return fail("a batch has a negative primitive count");
			primitives += static_cast<size_t>(b.opaque_count) + static_cast<size_t>(b.alpha_count);
			if (global) continue;
			if (b.part < last || b.part < 0 || static_cast<size_t>(b.part) >= m.parts.size())
				return fail("a render model's batches are not in part order");
			last = b.part;
			++part_batches[static_cast<size_t>(b.part)];
		}
		if (primitives != m.primitives.size()) return fail("a render model's batches do not count its primitives");
		for (const Primitive &p : m.primitives)
			if (p.indices.size() > 0xFFFFu) return fail("a primitive holds more than 65,535 indices");
		ByteWriter blob;
		Writer b{blob, error};
		for (const auto &p : m.attach_points) b.f32x3(p);
		for (size_t i = 0; i < m.parts.size(); ++i) {
			const Part &p = m.parts[i];
			blob.write_zeros(4);
			// A skinned model's parts each count one batch as well [orig: @ 0x44717B (df4oed)].
			blob.write_i32(global ? 1 : part_batches[i]);
			blob.write_zeros(4);
			blob.write_i32(p.parent);
			b.f32x3(p.rel);
			b.f32x3(p.abs);
			b.f32x3(p.centre);
			blob.write_i32(p.radius);
			blob.write_zeros(16);
		}
		if (global) {
			blob.write_zeros(4);
			blob.write_i32(static_cast<int32_t>(m.batches.size()));
			blob.write_zeros(20);
		}
		for (const Batch &batch : m.batches) {
			blob.write_zeros(4);
			blob.write_i32(batch.opaque_count);
			blob.write_zeros(4);
			blob.write_i32(batch.alpha_count);
			blob.write_zeros(16);
		}
		for (const Primitive &p : m.primitives) b.primitive(global, p);
		for (const Material &mat : m.materials)
			if (!b.material(mat)) return false;
		if (animated)
			for (const PartAnimation &a : m.part_animations) b.part_animation(a);
		w.write_u32(static_cast<uint32_t>(blob.size()));
		w.write_zeros(4);
		w.write_i32(static_cast<int32_t>(m.materials.size()));
		w.write_zeros(4);
		w.write_i32(static_cast<int32_t>(m.parts.size()));
		w.write_zeros(4);
		w.write_i32(static_cast<int32_t>(m.attach_points.size()));
		w.write_zeros(0x50 - 0x1C);
		w.write_u32(m.flags);
		w.write_zeros(0x60 - 0x54);
		w.write_u32(0);
		w.write_zeros(4);
		w.write_u32(0);
		w.write_zeros(0x88 - 0x6C);
		w.write_bytes(blob.data());
		return true;
	}

	// [orig: GP_WriteLights @ 0x4464E0 (df4oed)]: {256, size}, the 60-byte table (the model's
	// two ambient generators, which nothing of ours carries, -1.0 at +28 and the count), then
	// 48 bytes a light.
	void lights(const std::vector<Light> &lights) {
		w.write_u32(256);
		w.write_u32(static_cast<uint32_t>(60 + 48 * lights.size()));
		w.write_zeros(28);
		w.write_f32(-1.0f);
		w.write_zeros(4);
		w.write_u32(static_cast<uint32_t>(lights.size()));
		w.write_zeros(20);
		for (const Light &l : lights) {
			w.write_u8(l.style);
			w.write_u8(l.phase);
			w.write_u16(l.rate);
			w.write_u32(l.colour_start);
			w.write_u32(l.colour_end);
			f32x3(l.position);
			w.write_f32(l.atten_start);
			w.write_f32(l.atten_end);
			w.write_i32(l.part);
			w.write_zeros(12);
		}
	}

	// [orig: GP_WriteOcclusion @ 0x442110 (df4oed)]: the 60-byte records, then each one's
	// vertices, planes and faces.
	void occlusion(const std::vector<OcclusionObject> &objects) {
		for (const OcclusionObject &o : objects) {
			w.write_u8(o.type);
			w.write_u8(o.parent);
			w.write_u8(o.connecting);
			w.write_zeros(1);
			f32x3(o.centre);
			w.write_f32(o.radius);
			w.write_i32(static_cast<int32_t>(o.vertices.size()));
			w.write_zeros(4);
			w.write_i32(static_cast<int32_t>(o.planes.size()));
			w.write_zeros(4);
			w.write_i32(static_cast<int32_t>(o.faces.size()));
			w.write_zeros(20);
		}
		for (const OcclusionObject &o : objects) {
			for (const auto &v : o.vertices) f32x3(v);
			for (const auto &p : o.planes)
				for (float x : p) w.write_f32(x);
			for (const auto &f : o.faces)
				for (uint32_t x : f) w.write_u32(x);
		}
	}
};

} // namespace

bool write(const File &f, std::vector<uint8_t> &out, std::string &error) {
	out.clear();
	error.clear();
	ByteWriter bytes;
	Writer w{bytes, error};
	// [orig: GP_WriteModel @ 0x448F88..0x4493FC (df4oed)]: the section order parse() reads.
	if (!w.header(f) || !w.user_points(f.user_points) || !w.textures(f.textures)) return false;
	w.collision(f.header.kind, f.collision);
	if (!w.vertices(f.header.kind, f.vertices)) return false;
	for (const RenderModel &m : f.lods)
		if (!w.render_model(m)) return false;
	for (const std::string &name : f.control_registers) {
		if (!w.name(name, 16, "a CTRL register's name")) return false;
		bytes.write_zeros(28);
	}
	for (const auto &m : f.matrices)
		for (float v : m) bytes.write_f32(v);
	if (f.header.flags & kFlagLights) w.lights(f.lights);
	if (f.header.flags & kFlagVertexStream) {
		// {0, size, 0, 0}; the tool writes the size, 24 bytes a vertex [orig: @ 0x44933B (df4oed)].
		bytes.write_zeros(4);
		bytes.write_u32(static_cast<uint32_t>(f.vertex_stream.size()));
		bytes.write_zeros(8);
		bytes.write_bytes(f.vertex_stream);
	}
	if (f.header.flags & kFlagOcclusion) w.occlusion(f.occlusion);
	out = bytes.take();
	return true;
}

} // namespace opennova::threedi_gp
