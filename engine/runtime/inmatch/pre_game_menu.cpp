#include <runtime/inmatch/pre_game_menu.h>

#include <cstdio>

#include <base/io/le.h>

namespace opennova::inmatch {

namespace {

constexpr const char *kWindowNames[kPreGameWindowCount] = {
		"ERROR_WRAPPER",
		"ABORT_WRAPPER",
		"ABORTRETRY_WRAPPER",
		"GAME_PASSWORD_WRAPPER",
		"SPECTATE_WRAPPER",
		"TEAM_PASSWORD_WRAPPER",
		"TEAM_STATIC_WRAPPER",
		"SPECTATOR_STATIC_WRAPPER",
};

constexpr const char *kGenericStrings = "Generic Strings";

std::string gameerr_text(const rtxt::File *override_table, const rtxt::File *gameerr,
		const char *key) {
	return rtxt::lookup_with_override(override_table, gameerr, kGenericStrings, key);
}

// The engine's %ld / %2.2ld formats over a gameerr template (sprintf into the
// screen's message buffers). Only the conversions the queue lines carry are
// recognised; any other '%' sequence is copied through.
std::string format_longs(const std::string &format, const long *args, int arg_count) {
	std::string out;
	int next = 0;
	for (size_t i = 0; i < format.size(); ++i) {
		if (format[i] != '%') {
			out += format[i];
			continue;
		}
		const size_t rest = format.size() - i;
		const bool plain = rest >= 3 && format.compare(i, 3, "%ld") == 0;
		const bool padded = rest >= 6 && format.compare(i, 6, "%2.2ld") == 0;
		if ((plain || padded) && next < arg_count) {
			char buf[32];
			std::snprintf(buf, sizeof(buf), padded ? "%2.2ld" : "%ld", args[next++]);
			out += buf;
			i += padded ? 5 : 2;
			continue;
		}
		out += format[i];
	}
	return out;
}

} // namespace

const char *pre_game_window_name(int bit_index) {
	if (bit_index < 0 || bit_index >= kPreGameWindowCount) return "";
	return kWindowNames[bit_index];
}

uint32_t pre_game_windows(PreGamePanel panel) {
	switch (panel) {
	case PreGamePanel::Error:
	case PreGamePanel::Progress:
		return kPreGameErrorWrapper | kPreGameAbortWrapper;
	case PreGamePanel::GamePassword:
		return kPreGameGamePasswordWrapper | kPreGameAbortRetryWrapper;
	case PreGamePanel::Spectate:
		return kPreGameSpectateWrapper | kPreGameAbortRetryWrapper;
	case PreGamePanel::SpectatorPassword:
		return kPreGameTeamPasswordWrapper | kPreGameSpectatorStaticWrapper |
		       kPreGameAbortRetryWrapper;
	case PreGamePanel::TeamPassword:
		return kPreGameTeamPasswordWrapper | kPreGameTeamStaticWrapper |
		       kPreGameAbortRetryWrapper;
	}
	return 0;
}

void fold_join_queue_record(JoinQueueRecord &record, const uint8_t *data, size_t size,
		uint64_t now_ms) {
	const uint8_t queued = size >= 1 ? data[0] : 0;
	int32_t position = 0;
	int32_t length = 0;
	if (queued != 0) {
		position = size >= 3 ? io::read_u16_le(data + 1) : 0;
		length = size >= 5 ? io::read_u16_le(data + 3) : 0;
		// The stamp is taken while the PREVIOUS record was not queued.
		// [orig: `if (!queued) GetTickCount()` @0x4253e1..0x4253e9]
		if (!record.queued) record.queued_since_ms = now_ms;
	}
	record.position = position;
	record.length = length;
	record.queued = queued != 0;
}

std::string join_screen_text(JoinScreenStage stage, const JoinQueueRecord &queue,
		uint64_t now_ms, const rtxt::File *override_table, const rtxt::File *gameerr) {
	switch (stage) {
	case JoinScreenStage::Joining:
		return gameerr_text(override_table, gameerr, "PRE_JOININGSESSION");
	case JoinScreenStage::Connecting:
		return gameerr_text(override_table, gameerr, "PRE_CONNECTING");
	case JoinScreenStage::Verifying:
		return gameerr_text(override_table, gameerr, "PRE_VERIFYING");
	case JoinScreenStage::Starting:
		return gameerr_text(override_table, gameerr, "PRE_CONNECTED");
	case JoinScreenStage::Queued:
		break;
	}
	// A record with no valid position reads the plain wait line.
	// [orig: the queued / position > 0 / length >= position test @0x56a77d]
	if (!queue.queued || queue.position <= 0 || queue.length < queue.position)
		return gameerr_text(override_table, gameerr, "WAITTOJOIN");
	std::string line;
	if (queue.position > 2) {
		const long ahead = queue.position - 1;
		line = format_longs(gameerr_text(override_table, gameerr, "WAITXXXPEOPLEINFRONT1"),
				&ahead, 1);
	} else if (queue.position == 2) {
		const long ahead = 1;
		line = format_longs(gameerr_text(override_table, gameerr, "WAITXXXPEOPLEINFRONT2"),
				&ahead, 1);
	} else {
		line = gameerr_text(override_table, gameerr, "WAITXXXPEOPLEINFRONT3");
	}
	// The time queued, days dropped. [orig: Napi_DecomposeTime @0x56a7ab over
	// GetTickCount - the +0x300 stamp; WAITTIMEDISPLAY @0x56a81e..0x56a84f]
	const uint64_t waited_ms = now_ms >= queue.queued_since_ms ? now_ms - queue.queued_since_ms : 0;
	const uint64_t waited_s = waited_ms / 1000;
	const long hms[3] = {static_cast<long>((waited_s / 3600) % 24),
			static_cast<long>((waited_s / 60) % 60), static_cast<long>(waited_s % 60)};
	line += ' ';
	line += format_longs(gameerr_text(override_table, gameerr, "WAITTIMEDISPLAY"), hms, 3);
	return line;
}

} // namespace opennova::inmatch
