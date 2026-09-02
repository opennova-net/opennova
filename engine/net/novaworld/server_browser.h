#pragma once

// The NovaWorld server browser's table model over the GSB rows: the column
// vocabulary, the quick filters, the sort order and the cell/detail text.
// OpenNova UI policy (retail's browser is its own compiled menu; nothing here
// is a witness), headless-reproducible and therefore an engine fact the Godot
// panel only renders (ADR 0042 d2). The GSB text fields are the host's cp1252
// bytes (gsb.h); the filter's substring match and the sort keys fold case in
// ASCII only.

#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include <net/novaworld/gsb.h>

namespace opennova {

// The table's columns, in display order. Players and Ping sort numerically,
// Access on the lock predicate, the rest by text.
enum class BrowserColumn : int {
	Name = 0,
	Mission = 1,
	Type = 2,
	Players = 3,
	Ping = 4,
	Access = 5,
};
inline constexpr int kBrowserColumnCount = 6;

// A row whose ping sweep has not reported yet. The sweep's own codes
// (kPingFailed / kPingNeverAttempted, ping_sweep.h) are negative too; the
// browser shows every negative as unreachable and only this one as pending,
// and every negative sorts last in either direction.
inline constexpr int kPingPending = -1;

struct BrowserFilter {
	std::string text;       // case-insensitive substring over name / mission / mod ("" = any)
	std::string game_type;  // exact type, case-insensitive ("" = all)
	bool hide_full = false;
	bool hide_empty = false;
	bool hide_locked = false;
};

// True when the row advertises a password or a lock.
bool browser_row_is_locked(const GsbServerEntry &row);

// The Ping cell: pending shows "...", any other negative "N/A", else the
// round-trip in milliseconds.
std::string browser_ping_text(int ping);

// One table row's cells, in BrowserColumn order.
std::array<std::string, kBrowserColumnCount> browser_row_cells(const GsbServerEntry &row,
                                                               int ping);

// The indices of `rows` that pass `filter`, in input order.
std::vector<std::size_t> browser_filter_rows(const std::vector<GsbServerEntry> &rows,
                                             const BrowserFilter &filter);

// Reorder `order` (indices into `rows`) by `column`. `pings` is aligned with
// `rows` (kPingPending when unmeasured). Players and Ping compare
// numerically — an unmeasured or failed ping always sorts last, either
// direction — Access on the lock predicate, everything else case-insensitively
// with the server name as the tiebreak. Stable.
void browser_sort_rows(std::vector<std::size_t> &order, const std::vector<GsbServerEntry> &rows,
                       const std::vector<int> &pings, BrowserColumn column, bool ascending);

// The details pane's labeled lines (blank fields skipped; "Players: n/m"
// always; then the dedicated / PunkBuster / password flag lines).
std::vector<std::string> browser_details_lines(const GsbServerEntry &row);

// The row's hover text: mission, region, country, expansion and the address
// (an unreported 0.0.0.0 host is omitted), one per line.
std::string browser_row_tooltip(const GsbServerEntry &row);

} // namespace opennova
