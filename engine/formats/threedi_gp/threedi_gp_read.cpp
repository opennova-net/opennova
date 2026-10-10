// The GP reader: the BHD loader's read order, section by section, each record's words at
// the offsets its readers take them from (threedi_gp.h). Counts are checked against the
// bytes left before anything is sized, so a damaged file fails with where it ran out.

#include <formats/threedi_gp/threedi_gp.h>

#include <cstring>

#include <base/io/byte_reader.h>

namespace opennova::threedi_gp {
namespace {

using io::ByteReader;

// A fixed name field: the bytes up to its NUL.
std::string read_name(ByteReader &r, size_t width) {
	std::string out(width, '\0');
	r.read_bytes(reinterpret_cast<uint8_t *>(&out[0]), width);
	out.resize(std::strlen(out.c_str()));
	return out;
}

struct Parser {
	ByteReader &r;
	const uint8_t *data; // the bytes `r` reads, for a sub-block's own reader
	std::string &error;

	const uint8_t *here() const { return data + r.position(); }

	bool fail(const std::string &where) {
		error = where;
		return false;
	}
	// `count` records of `size` bytes fit in what is left.
	bool fits(int64_t count, size_t size, const char *what) {
		if (count < 0) return fail(std::string(what) + ": a negative count");
		if (static_cast<uint64_t>(count) > r.remaining() / size)
			return fail(std::string(what) + ": runs past the end of the file");
		return true;
	}
	std::array<int32_t, 3> i32x3() { return {r.read_i32(), r.read_i32(), r.read_i32()}; }
	std::array<float, 3> f32x3() { return {r.read_f32(), r.read_f32(), r.read_f32()}; }
	Generator generator() {
		Generator g;
		g.style = r.read_u8();
		g.param = r.read_u8();
		g.rate = r.read_i16();
		g.start = r.read_i16();
		g.end = r.read_i16();
		return g;
	}

	// [orig: GP_LoadModel @ 0x510E75..0x510F22 (dfbhd): the 0xEC header and its gates]
	bool header(Header &h) {
		if (r.remaining() < kHeaderSize) return fail("header: the file is shorter than 0xEC bytes");
		uint8_t magic[4];
		r.read_bytes(magic, 4);
		h.kind = detect(magic, 4);
		if (h.kind == Kind::None) return fail("header: not a GP model (no GPM, GPS or GPP magic)");
		if (magic[3] != kFormatVersion) return fail("header: format byte is not 2");
		h.revision = r.read_u32();
		if (h.revision > kMaxRevision) return fail("header: revision past 0x103");
		h.name = read_name(r, 16);
		h.flags = r.read_u32();
		h.lod_count = r.read_i32();
		if (h.lod_count < 1 || h.lod_count > kMaxLods) return fail("header: LOD count outside 1..4");
		for (int32_t &t : h.thresholds) t = r.read_i32();
		r.skip(16);
		for (uint32_t &t : h.tags) t = r.read_u32();
		r.skip(16);
		h.radius = r.read_i32();
		h.radius_xy = r.read_i32();
		h.height = r.read_i32();
		r.skip(0x88 - 0x6C);
		h.vertex_count = r.read_i32();
		r.skip(0xB0 - 0x8C);
		h.user_point_count = r.read_i32();
		r.skip(0xBC - 0xB4);
		h.control_register_count = r.read_i32();
		r.skip(4);
		h.matrix_count = r.read_i32();
		r.skip(0xD0 - 0xC8);
		h.occlusion_count = r.read_i32();
		r.skip(kHeaderSize - 0xD4);
		return true;
	}

	bool user_points(int32_t count, std::vector<UserPoint> &out) {
		if (!fits(count, 48, "user points")) return false;
		out.resize(static_cast<size_t>(count));
		for (UserPoint &p : out) {
			p.position = i32x3();
			p.direction = i32x3();
			p.part = r.read_i32();
			p.type = r.read_u8();
			r.skip(3);
			p.name = read_name(r, 16);
		}
		return true;
	}

	// [orig: GP_LoadModel @ 0x510F8D..0x510FB6 (dfbhd)]
	bool textures(std::vector<TextureRow> &out) {
		if (r.remaining() < 4) return fail("texture table: no count");
		const uint32_t count = r.read_u32();
		if (!fits(count, 60, "texture table")) return false;
		out.resize(count);
		for (TextureRow &t : out) {
			t.name = read_name(r, 16);
			t.alpha = read_name(r, 16);
			r.skip(4);
			t.id = r.read_i16();
			t.flags = r.read_u16();
			r.skip(20); // the size a failed load's checkerboard takes, then runtime handles
		}
		return true;
	}

	// [orig: GP_LoadCollisionModel @ 0x514D70 (dfbhd)]: a 0x88 header, then one blob holding
	// the vertices, normals, faces, sections, translations, planes and volumes in that order.
	bool collision(Collision &c) {
		if (r.remaining() < 0x88) return fail("collision: no header");
		r.skip(4); // the kind flag (no reader)
		const uint32_t blob = r.read_u32();
		r.skip(4);
		for (int32_t &v : c.radii) v = r.read_i32();
		for (int32_t &v : c.box) v = r.read_i32();
		int32_t counts[7];
		for (int32_t &n : counts) {
			n = r.read_i32();
			r.skip(4);
		}
		r.skip(0x88 - 0x68);
		static constexpr size_t kSizes[7] = {8, 8, 44, 128, 12, 16, 96};
		uint64_t total = 0;
		for (int i = 0; i < 7; ++i) {
			if (counts[i] < 0) return fail("collision: a negative count");
			total += static_cast<uint64_t>(counts[i]) * kSizes[i];
		}
		if (blob > r.remaining()) return fail("collision: the block runs past the end of the file");
		if (total > blob) return fail("collision: the records overrun the block");
		const size_t end = r.position() + blob;
		c.vertices.resize(static_cast<size_t>(counts[0]));
		for (CollisionVertex &v : c.vertices) {
			v.x = r.read_i16();
			v.y = r.read_i16();
			v.z = r.read_i16();
			v.section = r.read_i16();
		}
		c.normals.resize(static_cast<size_t>(counts[1]));
		for (CollisionNormal &n : c.normals) {
			n.x = r.read_i16();
			n.y = r.read_i16();
			n.z = r.read_i16();
			n.axis = r.read_i16();
		}
		c.faces.resize(static_cast<size_t>(counts[2]));
		for (CollisionFace &f : c.faces) {
			for (int16_t &k : f.corners) k = r.read_i16();
			f.normal = r.read_i16();
			f.plane = r.read_i32();
			for (int32_t &b : f.box) b = r.read_i32();
			f.flags = r.read_u32();
			f.surface = r.read_u8();
			r.skip(3);
		}
		c.sections.resize(static_cast<size_t>(counts[3]));
		for (CollisionSection &s : c.sections) {
			s.flags = r.read_i32();
			s.vertex_count = r.read_i32();
			r.skip(4);
			s.face_count = r.read_i32();
			r.skip(4);
			s.normal_count = r.read_i32();
			r.skip(4);
			s.volume_count = r.read_i32();
			r.skip(4);
			s.parent = r.read_i32();
			r.skip(12);
			s.offset = i32x3();
			for (int32_t &b : s.box) b = r.read_i32();
			s.centre = i32x3();
			s.radius = r.read_i32();
			r.skip(24);
		}
		c.translations.resize(static_cast<size_t>(counts[4]));
		for (auto &t : c.translations) t = i32x3();
		c.planes.resize(static_cast<size_t>(counts[5]));
		for (CollisionPlane &p : c.planes) {
			p.normal = i32x3();
			p.d = r.read_i32();
		}
		c.volumes.resize(static_cast<size_t>(counts[6]));
		for (CollisionVolume &v : c.volumes) {
			v.type = r.read_i32();
			v.flags = r.read_i32();
			r.skip(40);
			for (int32_t &b : v.box) b = r.read_i32();
			v.plane_count = r.read_i32();
			r.skip(20);
		}
		r.skip(end - r.position());
		return true;
	}

	// [orig: GP_LoadModel @ 0x511017..0x5112A0 (dfbhd)]
	bool vertices(Kind kind, int32_t count, std::vector<Vertex> &out) {
		const size_t stride = kind == Kind::Gpm ? 44 : kind == Kind::Gps ? 48 : 60;
		if (!fits(count, stride, "render vertices")) return false;
		out.resize(static_cast<size_t>(count));
		for (Vertex &v : out) {
			v.position = f32x3();
			if (kind == Kind::Gps) v.blend = r.read_f32();
			if (kind == Kind::Gpp) {
				v.weights = f32x3();
				for (uint8_t &p : v.palette) {
					p = r.read_u8();
					// [orig: @ 0x51127E..0x5112A0 (dfbhd): a slot past 15 fails the load]
					if (p > 15) return fail("render vertices: a palette slot past 15");
				}
			}
			v.normal = f32x3();
			r.skip(4); // the packed colour
			v.uv0 = {r.read_f32(), r.read_f32()};
			v.uv1 = {r.read_f32(), r.read_f32()};
		}
		return true;
	}

	bool primitive(bool global, Primitive &p) {
		const size_t head = global ? 48 : 40;
		if (r.remaining() < head) return fail("render model: a primitive runs past its block");
		p.material = r.read_i32();
		r.skip(4); // the "Addr" tag the loader overwrites with the index offset
		const uint16_t count = r.read_u16();
		r.skip(2);  // the triangle count, 0 on disk
		p.topology = r.read_i32();
		p.first_vertex = r.read_i32();
		p.vertex_word = r.read_i32();
		r.read_bytes(p.palette.data(), p.palette.size());
		if (global) {
			r.skip(7);
			p.palette_length = r.read_u8();
		}
		if (!fits(count, 2, "render model: indices")) return false;
		p.indices.resize(count);
		for (uint16_t &i : p.indices) i = r.read_u16();
		return true;
	}

	// [orig: GP_LoadRenderModel @ 0x5157F0 (dfbhd)]
	bool render_model(RenderModel &m) {
		if (r.remaining() < 0x88) return fail("render model: no header");
		const uint32_t blob = r.read_u32();
		r.skip(4);
		const int32_t material_count = r.read_i32();
		r.skip(4);
		const int32_t part_count = r.read_i32();
		r.skip(4);
		const int32_t point_count = r.read_i32();
		r.skip(0x50 - 0x1C);
		m.flags = r.read_u32();
		r.skip(0x60 - 0x54);
		m.extra_count = r.read_u32();
		r.skip(4);
		m.record_count = r.read_u32();
		r.skip(0x88 - 0x6C);
		if (blob > r.remaining()) return fail("render model: the block runs past the end of the file");
		ByteReader block(here(), blob);
		Parser b{block, here(), error};
		r.skip(blob);
		if (!b.fits(point_count, 12, "render model: attach points")) return false;
		block.skip(static_cast<size_t>(point_count) * 12);
		if (!b.fits(part_count, 72, "render model: parts")) return false;
		std::vector<int32_t> part_batches(static_cast<size_t>(part_count));
		m.parts.resize(static_cast<size_t>(part_count));
		for (size_t i = 0; i < m.parts.size(); ++i) {
			Part &p = m.parts[i];
			block.skip(4);
			part_batches[i] = block.read_i32();
			block.skip(4);
			p.parent = block.read_i32();
			p.rel = b.f32x3();
			p.abs = b.f32x3();
			p.centre = b.f32x3();
			p.radius = block.read_i32();
			block.skip(16);
		}
		const bool global = (m.flags & kRenderGlobalBatch) != 0;
		std::vector<int> batch_part; // the part each batch belongs to
		auto read_batches = [&](int32_t count, int part) {
			if (!b.fits(count, 32, "render model: batches")) return false;
			for (int32_t i = 0; i < count; ++i) {
				Batch batch;
				batch.part = part;
				block.skip(4);
				batch.opaque_count = block.read_i32();
				block.skip(4);
				batch.alpha_count = block.read_i32();
				block.skip(16);
				if (batch.opaque_count < 0 || batch.alpha_count < 0)
					return b.fail("render model: a negative primitive count");
				m.batches.push_back(batch);
				batch_part.push_back(part);
			}
			return true;
		};
		if (global) {
			// [orig: @ 0x515898..0x51590D (dfbhd): a 28-byte table header, its count at +4]
			if (block.remaining() < 28) return fail("render model: no global batch table");
			block.skip(4);
			const int32_t count = block.read_i32();
			block.skip(20);
			if (!read_batches(count, 0)) return false;
		} else {
			for (size_t i = 0; i < m.parts.size(); ++i)
				if (!read_batches(part_batches[i], static_cast<int>(i))) return false;
		}
		for (size_t i = 0; i < m.batches.size(); ++i) {
			const int32_t runs[2] = {m.batches[i].opaque_count, m.batches[i].alpha_count};
			for (int run = 0; run < 2; ++run)
				for (int32_t k = 0; k < runs[run]; ++k) {
					Primitive p;
					p.part = batch_part[i];
					p.alpha = run == 1;
					if (!b.primitive(global, p)) return false;
					m.primitives.push_back(std::move(p));
				}
		}
		if (!b.fits(material_count, 152, "render model: materials")) return false;
		m.materials.resize(static_cast<size_t>(material_count));
		for (Material &mat : m.materials) {
			block.skip(16); // the record's name: no reader
			mat.attributes = block.read_u32();
			block.skip(2);  // the flipbook rate: no runtime reader
			mat.frame_count = block.read_i8();
			mat.frame_counter = block.read_u8();
			block.skip(16); // the region colour (runtime) and its triplets
			for (int8_t &id : mat.region_textures) id = block.read_i8();
			mat.alpha_texture = block.read_i8();
			block.skip(4);  // the environment tint
			mat.env_pass = block.read_u8();
			block.skip(1);
			mat.shader = block.read_u8();
			block.skip(5);
			mat.colour_source = block.read_u8();
			mat.alpha_source = block.read_u8();
			mat.alpha = block.read_u8();
			mat.alpha_ref = block.read_u8();
			mat.blend = block.read_u8();
			mat.paired = block.read_u8();
			block.skip(18); // +0x3E..+0x50: unread, then the tilings no reader takes
			mat.u = b.generator();
			mat.v = b.generator();
			mat.rgb.style = block.read_u8();
			mat.rgb.param = block.read_u8();
			mat.rgb.rate = block.read_i16();
			block.read_bytes(mat.rgb.start.data(), 4);
			block.read_bytes(mat.rgb.end.data(), 4);
			mat.alpha_gen = b.generator();
			block.skip(152 - 0x74); // runtime slots
		}
		if (m.flags & kRenderPartAnimations) {
			if (!b.fits(part_count, 92, "render model: part animations")) return false;
			m.part_animations.resize(static_cast<size_t>(part_count));
			for (PartAnimation &a : m.part_animations) {
				a.flags = block.read_u32();
				a.parent = block.read_u8();
				a.part = block.read_u8();
				block.skip(2);
				a.frame = block.read_i32();
				// A spinner reads the first 12 track bytes as three floats.
				if (block.remaining() >= 12) std::memcpy(a.spin.data(), b.here(), 12);
				for (Generator &g : a.tracks) g = b.generator();
				block.skip(92 - 60);
			}
		}
		// The 88-byte primitives (+0x60) and 232-byte records (+0x68) BHD never ships follow;
		// the migration refuses a model that has them, so they are not read.
		return true;
	}

	bool control_registers(int32_t count, std::vector<std::string> &out) {
		if (!fits(count, 44, "CTRL registers")) return false;
		out.resize(static_cast<size_t>(count));
		for (std::string &name : out) {
			name = read_name(r, 16);
			r.skip(28);
		}
		return true;
	}

	bool matrices(int32_t count, std::vector<std::array<float, 16>> &out) {
		if (!fits(count, 64, "matrices")) return false;
		out.resize(static_cast<size_t>(count));
		for (auto &m : out)
			for (float &v : m) v = r.read_f32();
		return true;
	}

	// [orig: GP_LoadModel @ 0x511402..0x511476 (dfbhd)]: an outer {version, size}; a version
	// under 256 skips the size. The table: 36 bytes no reader takes, the count at +36, then 48
	// bytes a light past the 60-byte header.
	bool lights(std::vector<Light> &out) {
		if (r.remaining() < 8) return fail("lights: no header");
		const uint32_t version = r.read_u32();
		const uint32_t size = r.read_u32();
		if (size > r.remaining()) return fail("lights: the block runs past the end of the file");
		ByteReader block(here(), size);
		const uint8_t *block_data = here();
		r.skip(size);
		if (version < 256) return true;
		if (size < 60) return fail("lights: the table is shorter than its header");
		block.skip(36);
		const uint32_t count = block.read_u32();
		block.skip(20);
		Parser b{block, block_data, error};
		if (!b.fits(count, 48, "lights")) return false;
		out.resize(count);
		for (Light &l : out) {
			l.style = block.read_u8();
			l.phase = block.read_u8();
			l.rate = block.read_u16();
			l.colour_start = block.read_u32();
			l.colour_end = block.read_u32();
			l.position = b.f32x3();
			l.atten_start = block.read_f32();
			l.atten_end = block.read_f32();
			l.part = block.read_i32();
			block.skip(12);
		}
		return true;
	}

	// [orig: GP_LoadModel @ 0x511488..0x51151A (dfbhd)]: {0, size, 0, 0}, a zero size
	// meaning 24 bytes a vertex.
	bool vertex_stream(int32_t vertex_count, std::vector<uint8_t> &out) {
		if (r.remaining() < 16) return fail("vertex stream: no header");
		r.skip(4);
		uint32_t size = r.read_u32();
		r.skip(8);
		if (size == 0) size = static_cast<uint32_t>(vertex_count) * 24u;
		if (size > r.remaining()) return fail("vertex stream: runs past the end of the file");
		r.read_bytes(out, size);
		return true;
	}

	// [orig: GP_LoadOcclusionData @ 0x514FB0 (dfbhd)]: the records, then each one's
	// vertices, planes and faces in turn.
	bool occlusion(int32_t count, std::vector<OcclusionObject> &out) {
		if (!fits(count, 60, "occlusion")) return false;
		out.resize(static_cast<size_t>(count));
		std::vector<std::array<int32_t, 3>> counts(out.size());
		for (size_t i = 0; i < out.size(); ++i) {
			OcclusionObject &o = out[i];
			o.type = r.read_u8();
			o.parent = r.read_u8();
			o.connecting = r.read_u8();
			r.skip(17);
			counts[i][0] = r.read_i32();
			r.skip(4);
			counts[i][1] = r.read_i32();
			r.skip(4);
			counts[i][2] = r.read_i32();
			r.skip(20);
		}
		for (size_t i = 0; i < out.size(); ++i) {
			OcclusionObject &o = out[i];
			if (!fits(counts[i][0], 12, "occlusion vertices")) return false;
			o.vertices.resize(static_cast<size_t>(counts[i][0]));
			for (auto &v : o.vertices) v = f32x3();
			if (!fits(counts[i][1], 16, "occlusion planes")) return false;
			o.planes.resize(static_cast<size_t>(counts[i][1]));
			for (auto &p : o.planes) p = {r.read_f32(), r.read_f32(), r.read_f32(), r.read_f32()};
			if (!fits(counts[i][2], 12, "occlusion faces")) return false;
			o.faces.resize(static_cast<size_t>(counts[i][2]));
			for (auto &f : o.faces) f = {r.read_u32(), r.read_u32(), r.read_u32()};
		}
		return true;
	}
};

} // namespace

Kind detect(const uint8_t *data, size_t size) {
	if (data == nullptr || size < 3 || data[0] != 'G' || data[1] != 'P') return Kind::None;
	switch (data[2]) {
	case 'M': return Kind::Gpm;
	case 'S': return Kind::Gps;
	case 'P': return Kind::Gpp;
	default: return Kind::None;
	}
}

bool parse(const uint8_t *data, size_t size, File &out, std::string &error) {
	out = File{};
	error.clear();
	ByteReader r(data, data != nullptr ? size : 0);
	Parser p{r, data, error};
	if (!p.header(out.header)) return false;
	const Header &h = out.header;
	if (!p.user_points(h.user_point_count, out.user_points)) return false;
	if (!p.textures(out.textures)) return false;
	if (!p.collision(out.collision)) return false;
	if (!p.vertices(h.kind, h.vertex_count, out.vertices)) return false;
	out.lods.resize(static_cast<size_t>(h.lod_count));
	for (RenderModel &m : out.lods)
		if (!p.render_model(m)) return false;
	if (!p.control_registers(h.control_register_count, out.control_registers)) return false;
	if (!p.matrices(h.matrix_count, out.matrices)) return false;
	if ((h.flags & kFlagLights) && !p.lights(out.lights)) return false;
	if ((h.flags & kFlagVertexStream) && !p.vertex_stream(h.vertex_count, out.vertex_stream)) return false;
	if ((h.flags & kFlagOcclusion) && !p.occlusion(h.occlusion_count, out.occlusion)) return false;
	if (!r.ok()) return p.fail("the file ends inside a record");
	return true;
}

} // namespace opennova::threedi_gp
