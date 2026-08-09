#include "nova_particle_table.h"

#include <algorithm>
#include <cstdint>

using namespace godot;

namespace {
constexpr int ROWS = 32;
constexpr int COLS = 8;
constexpr int TOTAL = ROWS * COLS;
} // namespace

ParticleTable::ParticleTable() {
	data.resize(TOTAL);
	for (int i = 0; i < TOTAL; ++i) {
		data[i] = 0;
	}
}

void ParticleTable::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_id", "value"), &ParticleTable::set_id);
	ClassDB::bind_method(D_METHOD("get_id"), &ParticleTable::get_id);
	ClassDB::bind_method(D_METHOD("set_data", "value"), &ParticleTable::set_data);
	ClassDB::bind_method(D_METHOD("get_data"), &ParticleTable::get_data);
	ClassDB::bind_method(D_METHOD("row_count"), &ParticleTable::row_count);
	ClassDB::bind_method(D_METHOD("get_row", "row"), &ParticleTable::get_row);
	ClassDB::bind_method(D_METHOD("set_row", "row", "values"), &ParticleTable::set_row);
	ClassDB::bind_method(D_METHOD("sample", "t"), &ParticleTable::sample);

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "id"), "set_id", "get_id");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_BYTE_ARRAY, "data"), "set_data", "get_data");
}

void ParticleTable::set_id(const String &p_value) {
	id = p_value;
	emit_changed();
}
String ParticleTable::get_id() const { return id; }

void ParticleTable::set_data(const PackedByteArray &p_data) {
	data = p_data;
	if (data.size() != TOTAL) {
		const int prior = data.size();
		data.resize(TOTAL);
		for (int i = prior; i < TOTAL; ++i) {
			data[i] = 0;
		}
	}
	emit_changed();
}

PackedByteArray ParticleTable::get_data() const { return data; }

int ParticleTable::row_count() const {
	return ROWS;
}

PackedByteArray ParticleTable::get_row(int row) const {
	PackedByteArray out;
	out.resize(COLS);
	if (row < 0 || row >= ROWS) {
		for (int i = 0; i < COLS; ++i) out[i] = 0;
		return out;
	}
	for (int i = 0; i < COLS; ++i) {
		out[i] = data[row * COLS + i];
	}
	return out;
}

void ParticleTable::set_row(int row, const PackedByteArray &values) {
	if (row < 0 || row >= ROWS) {
		return;
	}
	const int n = std::min<int>(values.size(), COLS);
	for (int i = 0; i < n; ++i) {
		data[row * COLS + i] = values[i];
	}
	emit_changed();
}

int ParticleTable::sample(float t) const {
	if (data.size() < TOTAL) {
		return 0;
	}
	// Engine: `BuildBillboardQuads @ 0x5e6d60` reads `lut[(int)(t * 256) & 0xFF]`
	// — flat byte lookup, no interpolation. The prior linear-interp variant
	// drifted from engine output by up to ~1 byte at midtones. Integer
	// indexing matches the engine exactly.
	const float clamped = std::clamp(t, 0.0f, 1.0f);
	const int index = static_cast<int>(clamped * static_cast<float>(TOTAL)) & 0xFF;
	return data[index];
}

void ParticleTable::copy_from_native(const opennova::particle::TableDef &table) {
	id = String::utf8(table.id.c_str());
	data.resize(TOTAL);
	for (int i = 0; i < TOTAL; ++i) data[i] = 0;
	const int rows_to_copy = std::min<int>(static_cast<int>(table.rows.size()), ROWS);
	for (int r = 0; r < rows_to_copy; ++r) {
		for (int c = 0; c < COLS; ++c) {
			data[r * COLS + c] = table.rows[static_cast<size_t>(r)][static_cast<size_t>(c)];
		}
	}
}

opennova::particle::TableDef ParticleTable::to_native() const {
	opennova::particle::TableDef out;
	out.id = id.utf8().get_data();
	out.rows.reserve(ROWS);
	for (int r = 0; r < ROWS; ++r) {
		std::array<std::uint8_t, 8> row{};
		for (int c = 0; c < COLS; ++c) {
			row[static_cast<size_t>(c)] = data[r * COLS + c];
		}
		out.rows.push_back(row);
	}
	return out;
}
