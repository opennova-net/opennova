#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <net/novaworld/ping_sweep.h>
#include <net/novaworld/server_browser.h>

#include "network/novaworld_server_row.h"

namespace godot {

// The server browser's table model (engine/net/novaworld/server_browser.h),
// bound as statics over NovaWorldServerRow: the panel renders what these
// return and owns nothing but the Tree/Label writes.
class NovaWorldServerBrowser : public RefCounted {
	GDCLASS(NovaWorldServerBrowser, RefCounted)

protected:
	static void _bind_methods();

public:
	enum Column {
		COLUMN_NAME = static_cast<int>(opennova::BrowserColumn::Name),
		COLUMN_MISSION = static_cast<int>(opennova::BrowserColumn::Mission),
		COLUMN_TYPE = static_cast<int>(opennova::BrowserColumn::Type),
		COLUMN_PLAYERS = static_cast<int>(opennova::BrowserColumn::Players),
		COLUMN_PING = static_cast<int>(opennova::BrowserColumn::Ping),
		COLUMN_ACCESS = static_cast<int>(opennova::BrowserColumn::Access),
	};
	// The ping sweep's row states (engine/net/novaworld/ping_sweep.h) plus the
	// browser's own pending sentinel; a non-negative ping is the round-trip in ms.
	enum PingState {
		PING_PENDING = opennova::kPingPending,
		PING_FAILED = opennova::kPingFailed,
		PING_NEVER_ATTEMPTED = opennova::kPingNeverAttempted,
	};

	static String ping_text(int p_ping);
	static PackedStringArray row_cells(const Ref<NovaWorldServerRow> &p_row, int p_ping);
	static TypedArray<NovaWorldServerRow> filter_rows(const TypedArray<NovaWorldServerRow> &p_rows,
			const String &p_text, const String &p_game_type, bool p_hide_full, bool p_hide_empty,
			bool p_hide_locked);
	// `pings` maps rid -> ping (a rid without an entry is PING_PENDING).
	static TypedArray<NovaWorldServerRow> sort_rows(const TypedArray<NovaWorldServerRow> &p_rows,
			Column p_column, bool p_ascending, const Dictionary &p_pings);
	static PackedStringArray details_lines(const Ref<NovaWorldServerRow> &p_row);
	static String row_tooltip(const Ref<NovaWorldServerRow> &p_row);
};

} // namespace godot

VARIANT_ENUM_CAST(godot::NovaWorldServerBrowser::Column);
VARIANT_ENUM_CAST(godot::NovaWorldServerBrowser::PingState);
