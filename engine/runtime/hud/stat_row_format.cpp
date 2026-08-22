#include <hud/stat_row_format.h>

#include <cstdio>

namespace opennova::hud {

std::string stat_format_time(int32_t seconds) {
	// [orig: the sprintf pair — value / 60 and value % 60]. Minutes unpadded,
	// seconds zero-padded to two.
	//
	// A negative value would give a nonsense "-1:-1" through the same
	// arithmetic; the board never carries one, so it is clamped at zero here
	// rather than being rendered.
	if (seconds < 0) seconds = 0;
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%d:%02d", seconds / 60, seconds % 60);
	return std::string(buf);
}

std::string stat_format_value(int32_t value, StatFieldKind kind, bool present) {
	// The unset case comes FIRST: a field with no entry is a dash whatever its
	// type, so a missing time does not render "0:00".
	if (!present) return std::string(kStatUnset);
	switch (kind) {
	case StatFieldKind::Time:
		return stat_format_time(value);
	case StatFieldKind::Integer: {
		char buf[32];
		std::snprintf(buf, sizeof(buf), "%d", value);
		return std::string(buf);
	}
	default:
		// An unrecognised field type is visible rather than guessed.
		return std::string(kStatUnknown);
	}
}

std::string stat_field_key(int field_id, bool large_layout, bool fallback) {
	char buf[64];
	const char *prefix = fallback ? "!" : "";
	if (large_layout) {
		std::snprintf(buf, sizeof(buf), "%sSTROVER_STATFIELD%02d", prefix, field_id);
	} else {
		std::snprintf(buf, sizeof(buf), "%sSTROVER_STATFIELDSMALL%02d", prefix,
				field_id);
	}
	return std::string(buf);
}

std::string stat_field_unknown_key(int field_id) {
	char buf[64];
	std::snprintf(buf, sizeof(buf), "Unk entry %d", field_id);
	return std::string(buf);
}

} // namespace opennova::hud
