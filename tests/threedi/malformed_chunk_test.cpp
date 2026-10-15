// The 3DI3 reader refuses a chunk whose count, record size or length disagree with each
// other, and it reads nothing past the chunk. Asserts used to guard these, and release builds
// compile them out. The editor reads every model in a project, and a file another tool saved
// badly used to crash it. Each case below builds a minimal valid model in memory and then
// spoils one chunk.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <formats/threedi/threedi_3di3.h>

using namespace opennova::threedi;

static int failures = 0;

static void check(bool condition, const std::string &message) {
	if (!condition) {
		std::fprintf(stderr, "FAIL: %s\n", message.c_str());
		++failures;
	}
}

using Bytes = std::vector<uint8_t>;

static void put_u32(Bytes &out, uint32_t v) {
	for (int i = 0; i < 4; ++i)
		out.push_back((uint8_t)(v >> (8 * i)));
}

static Bytes chunk(const char id[5], const Bytes &data, bool parent = false) {
	Bytes out(id, id + 4);
	put_u32(out, (uint32_t)data.size() | (parent ? THREEDI_3DI3_PARENT_FLAG : 0u));
	out.insert(out.end(), data.begin(), data.end());
	return out;
}

static Bytes parent(const char id[5], const std::vector<Bytes> &children) {
	Bytes data;
	for (const Bytes &child : children)
		data.insert(data.end(), child.begin(), child.end());
	return chunk(id, data, true);
}

// A counted chunk's payload: count, record size, then `body` bytes (zeroed records).
static Bytes counted(uint32_t count, uint32_t record_size, size_t body) {
	Bytes out;
	put_u32(out, count);
	put_u32(out, record_size);
	out.resize(out.size() + body, 0);
	return out;
}

static Bytes vert_data(uint32_t count, size_t records) {
	Bytes out;
	put_u32(out, count);
	put_u32(out, 40); // the plain stride: no skin, no tangents
	put_u32(out, 0);
	out.resize(out.size() + records * 40, 0);
	return out;
}

// The file around one RLOD's extra children and the root's extra chunks.
static Bytes model(const Bytes &vert, const Bytes &indx, const std::vector<Bytes> &lod_extra,
		const std::vector<Bytes> &root_extra) {
	std::vector<Bytes> lod = { chunk("VERT", vert), chunk("INDX", indx) };
	lod.insert(lod.end(), lod_extra.begin(), lod_extra.end());
	std::vector<Bytes> root = { parent("RLOD", lod) };
	root.insert(root.end(), root_extra.begin(), root_extra.end());
	Bytes file = { '3', 'D', 'I', '3' };
	put_u32(file, 3);
	const Bytes body = parent("ROOT", root);
	file.insert(file.end(), body.begin(), body.end());
	return file;
}

static Bytes good_vert() {
	return vert_data(1, 1);
}
static Bytes good_indx() {
	return counted(1, 2, 2);
}

static int read_rc(const Bytes &file) {
	Threedi3di3 m = {};
	const int rc = threedi_3di3_read_memory(file.data(), file.size(), &m);
	if (rc == 0)
		threedi_3di3_free(&m);
	return rc;
}

static void expect_refused(const std::string &what, const Bytes &file) {
	check(read_rc(file) != 0, what + " is refused");
}

static void expect_read(const std::string &what, const Bytes &file) {
	check(read_rc(file) == 0, what + " reads");
}

// A material record: zeroed but for its texture count.
static Bytes mtrl_data(uint32_t tex_count) {
	Bytes out = counted(1, 584, 584);
	const size_t at = 8 + 32;
	for (int i = 0; i < 4; ++i)
		out[at + i] = (uint8_t)(tex_count >> (8 * i));
	return out;
}

int main() {
	expect_read("the minimal model", model(good_vert(), good_indx(), {}, {}));

	// The LOD's own chunks.
	expect_refused("a VERT shorter than its header", model(Bytes(8, 0), good_indx(), {}, {}));
	expect_refused("a VERT counting more vertices than it holds",
			model(vert_data(2, 1), good_indx(), {}, {}));
	expect_refused("an INDX shorter than its header", model(good_vert(), Bytes(4, 0), {}, {}));
	expect_refused("an INDX counting more indices than it holds",
			model(good_vert(), counted(3, 2, 2), {}, {}));
	expect_refused(
			"an INDX with another record size", model(good_vert(), counted(1, 4, 4), {}, {}));
	expect_refused("a STRP stepping by a record size it does not read",
			model(good_vert(), good_indx(), { chunk("STRP", counted(1, 4, 4)) }, {}));
	expect_refused("a STRP counting more strips than it holds",
			model(good_vert(), good_indx(), { chunk("STRP", counted(2, 48, 48)) }, {}));
	expect_read("a STRP that fits",
			model(good_vert(), good_indx(), { chunk("STRP", counted(1, 48, 48)) }, {}));
	expect_refused("a ROBJ counting more objects than it holds",
			model(good_vert(), good_indx(), { chunk("ROBJ", counted(2, 52, 52)) }, {}));
	expect_refused("a PANM counting more parts than it holds",
			model(good_vert(), good_indx(), { chunk("PANM", counted(2, 68, 68)) }, {}));

	// The model's chunks, each a counted record table with the record size its parser reads.
	struct Table {
		const char *id;
		uint32_t record_size;
	};
	const Table tables[] = {
		{ "LGHT", 116 },
		{ "USRP", 48 },
		{ "BPLN", 12 },
		{ "BVOL", 36 },
		{ "CFAC", 44 },
		{ "COBJ", 88 },
		{ "CXLT", 12 },
		{ "OPLN", 16 },
		{ "OFAC", 12 },
		{ "OOBJ", 36 },
		{ "MTRX", 64 },
		{ "OVRT", 12 },
		{ "CTRL", 24 },
		{ "PANM", 68 },
		{ "MTRL", 584 },
	};
	for (const Table &t : tables) {
		const std::string id = t.id;
		expect_read(id + " with one record",
				model(good_vert(), good_indx(), {},
						{ chunk(t.id, counted(1, t.record_size, t.record_size)) }));
		expect_refused(id + " shorter than its header",
				model(good_vert(), good_indx(), {}, { chunk(t.id, Bytes(4, 0)) }));
		expect_refused(id + " counting more records than it holds",
				model(good_vert(), good_indx(), {},
						{ chunk(t.id, counted(2, t.record_size, t.record_size)) }));
		expect_refused(id + " with a record size larger than its parser's",
				model(good_vert(), good_indx(), {},
						{ chunk(t.id, counted(1, t.record_size * 4, t.record_size)) }));
		expect_refused(id + " with a record size of 0 and a count",
				model(good_vert(), good_indx(), {}, { chunk(t.id, counted(1000000, 0, 0)) }));
	}

	// CVRT and CNRM may carry padding past their records, and a record size of 0 with none.
	for (const char *id : { "CVRT", "CNRM" }) {
		const std::string name = id;
		expect_read(name + " padded past its record",
				model(good_vert(), good_indx(), {}, { chunk(id, counted(1, 8, 12)) }));
		expect_read(name + " empty with a record size of 0",
				model(good_vert(), good_indx(), {}, { chunk(id, counted(0, 0, 0)) }));
		expect_refused(name + " shorter than its header",
				model(good_vert(), good_indx(), {}, { chunk(id, Bytes(4, 0)) }));
		expect_refused(name + " counting more records than it holds",
				model(good_vert(), good_indx(), {}, { chunk(id, counted(2, 8, 8)) }));
		expect_refused(name + " with a record size of 0 and a count",
				model(good_vert(), good_indx(), {}, { chunk(id, counted(1000000, 0, 0)) }));
	}

	expect_refused("a CMDL shorter than its 64 bytes",
			model(good_vert(), good_indx(), {}, { chunk("CMDL", Bytes(8, 0)) }));
	expect_read("a CMDL of 64 bytes",
			model(good_vert(), good_indx(), {}, { chunk("CMDL", Bytes(64, 0)) }));
	expect_refused("a material naming more than 24 textures",
			model(good_vert(), good_indx(), {}, { chunk("MTRL", mtrl_data(25)) }));

	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("threedi malformed chunks: ok\n");
	return 0;
}
