// The host-initiated close and the two anti-cheat CRC challenges a stock host streams at a joiner.
//
// A live retail 1.7.5.7 co-op host closed an OpenNova joiner parked at the deploy screen after six
// minutes by sending ONE connection-description record (settings flag + tag 3), then went silent.
// The captured 80-byte body is pinned here byte for byte, driven through the joiner's real receive
// path, and asserted to raise session loss once with the decoded reason — while ordinary
// settings-update traffic keeps its behavior. The same session showed 37x S2C 0x30 and 36x S2C 0x31
// challenges; answering them with a value we cannot honestly compute is what a later live run proved
// fatal ("PUNT ACRC" / "PUNT WCRC"), so the SILENCE is pinned here instead (D-NET-181).

#include <npruntime/joiner_connection.h>

#include <npwire/nw_session_framing.h>
#include <npwire/protocol_message.h>
#include <npwire/session_hello.h>
#include <npwire/session_keys.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace opennova;
namespace np = opennova::np;

constexpr uint32_t kClientKey = 0x11223344u;
constexpr uint32_t kServerKey = 0x55667788u;
const std::string kClientScrk = "CLIENT-PUNT-SCRK";
const std::string kServerScrk = "SERVER-PUNT-SCRK";

// The flag byte a retail host stamps on this record: the high/settings bit plus LEN8 for a body
// under 256 bytes. [orig: NapiNPMessage_Create(msg_id 3, msg_class 1) @0x627fc0 —
// len_field_size 3 @0x628316]
constexpr uint8_t kDescriptionFlags = 0xA0;

// The kick exactly as captured off the wire (nw_pp prints it as tag=0x1103): a flat run of
// NAME 0x00 [u16 LE size] [size bytes]. DSTR "t35" is sprintf("t%d", 35) and DPC 33 / DDSTR
// "LogPuntEvent" are the two literals of the punt helper, so this body identifies its sender
// exactly. [orig: Server_LogCRCMismatchPunt @0x517ed0 (the "t%d" format @0x7cfb9c, the 33 and
// "LogPuntEvent" arguments @0x517f5a); the mismatchType 35 call site is the six-minute
// deploy-screen idle timeout Server_TickUpdate @0x51e109 (0x57E40 ms) -> @0x51e13a]
const std::vector<uint8_t> kCapturedPunt = {
	0x44, 0x53, 0x00, 0x04, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x44, 0x43, 0x00, 0x04, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x44, 0x50, 0x31, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x44, 0x50, 0x32, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x44, 0x53, 0x54, 0x52, 0x00, 0x04, 0x00, 0x74, 0x33, 0x35, 0x00,
	0x44, 0x50, 0x43, 0x00, 0x04, 0x00, 0x21, 0x00, 0x00, 0x00,
	0x44, 0x44, 0x53, 0x54, 0x52, 0x00, 0x0D, 0x00,
	0x4C, 0x6F, 0x67, 0x50, 0x75, 0x6E, 0x74, 0x45, 0x76, 0x65, 0x6E, 0x74, 0x00,
};

bool expect(bool condition, const char *message) {
	if (condition) return true;
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

void seed_joiner(np::JoinerConnection &joiner) {
	joiner.seed_in_match(kServerKey, kClientKey, kClientScrk, kServerScrk,
	                     1, 0, 0x0001, 0x14B9);
}

std::vector<uint8_t> frame_s2c(SessionSequencing &sequencing,
		std::vector<ProtocolMessage> messages) {
	std::vector<uint8_t> body;
	if (!frame_session_packet(sequencing, SessionCrypto{kServerScrk, {}, kClientKey},
	                          messages, body)) {
		return {};
	}
	return nw_encode_outbound(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(body));
}

// One structural CS control record — the initial settings form (low tag 0, high flag set, so full
// tag 0x100): [direction][u32 field mask][one u32 per set bit].
ProtocolMessage make_cs_config(uint8_t direction, uint8_t field, uint32_t value) {
	std::vector<uint8_t> payload{direction};
	const uint32_t mask = uint32_t{1} << field;
	for (int shift = 0; shift < 32; shift += 8)
		payload.push_back(static_cast<uint8_t>((mask >> shift) & 0xFFu));
	for (int shift = 0; shift < 32; shift += 8)
		payload.push_back(static_cast<uint8_t>((value >> shift) & 0xFFu));
	return make_protocol_message(0x00, std::move(payload), kDescriptionFlags);
}

void append_tlv(std::vector<uint8_t> &body, const char *name,
		const std::vector<uint8_t> &value) {
	for (const char *p = name; *p; ++p) body.push_back(static_cast<uint8_t>(*p));
	body.push_back(0);
	body.push_back(static_cast<uint8_t>(value.size() & 0xFFu));
	body.push_back(static_cast<uint8_t>((value.size() >> 8) & 0xFFu));
	body.insert(body.end(), value.begin(), value.end());
}

std::vector<uint8_t> u32_value(uint32_t value) {
	return {static_cast<uint8_t>(value & 0xFFu),
	        static_cast<uint8_t>((value >> 8) & 0xFFu),
	        static_cast<uint8_t>((value >> 16) & 0xFFu),
	        static_cast<uint8_t>((value >> 24) & 0xFFu)};
}

bool check_captured_punt_decodes_field_for_field() {
	DisconnectEvent event;
	if (!expect(kCapturedPunt.size() == 80,
	            "the captured punt body is the 80 bytes the host sent"))
		return false;
	if (!expect(parse_disconnect_event(
			kCapturedPunt.data(), kCapturedPunt.size(), event),
	            "the captured punt body parses as a disconnect block"))
		return false;
	return expect(event.ds == 1 && event.dc == 2 && event.dp1 == 0 && event.dp2 == 0 &&
	                      event.dstr == "t35" && event.dpc == 33 &&
	                      event.ddstr == "LogPuntEvent",
	              "every captured field decodes to its witnessed value");
}

bool check_decode_is_order_and_shape_robust() {
	// Reverse order with an unknown name wedged in the middle: retail keys on the name and skips
	// what it does not know by its length, so the same values must come back.
	std::vector<uint8_t> shuffled;
	append_tlv(shuffled, "DDSTR", {'L', 'o', 'g', 'P', 'u', 'n', 't', 'E', 'v', 'e', 'n', 't', 0});
	append_tlv(shuffled, "DPC", u32_value(33));
	append_tlv(shuffled, "ZZZZ", {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x11});
	append_tlv(shuffled, "DSTR", {'t', '3', '5', 0});
	append_tlv(shuffled, "DP2", u32_value(0));
	append_tlv(shuffled, "DP1", u32_value(0));
	append_tlv(shuffled, "DC", u32_value(2));
	append_tlv(shuffled, "DS", u32_value(1));
	DisconnectEvent shuffled_event;
	if (!expect(parse_disconnect_event(
			shuffled.data(), shuffled.size(), shuffled_event) &&
	                    shuffled_event.ds == 1 && shuffled_event.dc == 2 &&
	                    shuffled_event.dstr == "t35" && shuffled_event.dpc == 33 &&
	                    shuffled_event.ddstr == "LogPuntEvent",
	            "field order is not assumed and an unknown name is skipped by its length"))
		return false;

	// An empty name terminates the walk before its length is read: everything after it is gone.
	std::vector<uint8_t> terminated;
	append_tlv(terminated, "DC", u32_value(2));
	terminated.push_back(0);
	append_tlv(terminated, "DPC", u32_value(33));
	DisconnectEvent terminated_event;
	if (!expect(parse_disconnect_event(
			terminated.data(), terminated.size(), terminated_event) &&
	                    terminated_event.dc == 2 && terminated_event.dpc == 0,
	            "an empty name ends the walk and later fields are not read"))
		return false;

	// Every truncation of the captured body must be rejected or decoded, never read past the end.
	for (std::size_t length = 0; length < kCapturedPunt.size(); ++length) {
		const std::vector<uint8_t> truncated(
				kCapturedPunt.begin(), kCapturedPunt.begin() + static_cast<long>(length));
		DisconnectEvent truncated_event;
		parse_disconnect_event(truncated.data(), truncated.size(), truncated_event);
	}
	// A body whose final TLV claims more bytes than remain is malformed, not a short read.
	std::vector<uint8_t> overrun;
	append_tlv(overrun, "DC", u32_value(2));
	overrun.push_back('D');
	overrun.push_back('P');
	overrun.push_back('C');
	overrun.push_back(0);
	overrun.push_back(0x40);
	overrun.push_back(0x00);
	overrun.push_back(0x01);
	DisconnectEvent overrun_event;
	if (!expect(!parse_disconnect_event(overrun.data(), overrun.size(), overrun_event),
	            "a value that claims more bytes than remain is rejected"))
		return false;

	// A well-formed TLV run with none of the seven names is not a disconnect block — the shape
	// gate the dispatcher relies on to leave other settings traffic alone.
	std::vector<uint8_t> foreign;
	append_tlv(foreign, "XX", u32_value(7));
	DisconnectEvent foreign_event;
	return expect(!parse_disconnect_event(foreign.data(), foreign.size(), foreign_event),
	              "a TLV run carrying no known field is not a disconnect block");
}

bool check_captured_punt_closes_a_joiner_parked_at_the_deploy_screen() {
	np::JoinerConnection joiner("PuntedPlayer");
	seed_joiner(joiner);
	// The captured situation: an established joiner sitting on the deploy screen, owing a pick.
	if (!expect(joiner.begin_redeployment() && joiner.deployment_pick_pending(),
	            "the joiner is parked at the deploy screen before the kick"))
		return false;

	SessionSequencing server_tx{1, 0};
	const std::vector<uint8_t> kick = frame_s2c(server_tx,
			{make_protocol_message(0x03, kCapturedPunt, kDescriptionFlags)});
	if (!expect(!kick.empty(), "frame the captured kick as an S2C packet")) return false;

	const np::JoinerConnection::PollResult result =
			joiner.handle_datagram(kick.data(), kick.size());
	if (!expect(result.outbound.empty() && result.queued_send_messages.empty(),
	            "a closed session produces no further C2S traffic from the receive path"))
		return false;
	const std::string reason = joiner.session_loss_reason();
	if (!expect(joiner.session_lost() &&
	                    reason.find("33") != std::string::npos &&
	                    reason.find("LogPuntEvent") != std::string::npos &&
	                    reason.find("t35") != std::string::npos,
	            "session loss is raised carrying the decoded reason code and strings"))
		return false;
	if (!expect(joiner.phase() == np::JoinerConnection::Phase::Error &&
	                    !joiner.deployment_pick_pending() &&
	                    joiner.frame_deployment_pick(0xFFFF).empty(),
	            "the connection is terminal and the deploy screen stops accepting picks"))
		return false;
	if (!expect(joiner.pump(1).empty(),
	            "a closed session is no longer pumped"))
		return false;

	// Retail stores the event only while its slot is empty, so the FIRST record wins.
	SessionSequencing repeat_tx{2, 0};
	std::vector<uint8_t> second_body;
	append_tlv(second_body, "DC", u32_value(4));
	append_tlv(second_body, "DPC", u32_value(46));
	append_tlv(second_body, "DDSTR", {'L', 'a', 't', 'e', 'r', 0});
	const std::vector<uint8_t> second_kick = frame_s2c(repeat_tx,
			{make_protocol_message(0x03, second_body, kDescriptionFlags)});
	joiner.handle_datagram(second_kick.data(), second_kick.size());
	return expect(joiner.session_loss_reason() == reason,
	              "a later disconnect record cannot restate the cause");
}

bool check_ordinary_settings_traffic_is_undisturbed() {
	np::JoinerConnection joiner("SettingsTraffic");
	seed_joiner(joiner);
	SessionSequencing server_tx{1, 0};
	// The initial connection-control pair (full tag 0x100) plus the send-holdoff field the joiner
	// reads out of the CLIENT-direction record.
	const std::vector<uint8_t> settings = frame_s2c(server_tx,
			{make_cs_config(0, 3, 40), make_cs_config(1, 3, 250)});
	if (!expect(!settings.empty(), "frame the initial settings pair")) return false;
	const np::JoinerConnection::PollResult result =
			joiner.handle_datagram(settings.data(), settings.size());
	if (!expect(!joiner.session_lost() && joiner.session_loss_reason().empty() &&
	                    joiner.phase() == np::JoinerConnection::Phase::InMatch,
	            "settings-update traffic does not close the session"))
		return false;
	if (!expect(result.send_holdoff_set && result.send_holdoff == 250,
	            "the settings pre-pass still reads the client-direction send holdoff"))
		return false;

	// A settings-flagged tag 3 whose body is not a TLV run is not a disconnect: the gate keys on
	// the body parsing, not the tag alone.
	SessionSequencing noise_tx{2, 0};
	const std::vector<uint8_t> noise = frame_s2c(noise_tx,
			{make_protocol_message(0x03, {0xFF, 0xFE, 0xFD, 0xFC}, kDescriptionFlags)});
	joiner.handle_datagram(noise.data(), noise.size());
	return expect(!joiner.session_lost() &&
	                      joiner.phase() == np::JoinerConnection::Phase::InMatch,
	              "a tag-3 body that is not a disconnect block leaves the session open");
}

bool reply_body(const np::JoinerConnection::PollResult &result, uint8_t tag,
		std::vector<uint8_t> &body_out) {
	for (const ProtocolMessage &message : result.queued_send_messages) {
		if (message.flags.settings_update || message.tag != tag) continue;
		body_out = message.payload;
		return true;
	}
	return false;
}

// The anti-cheat CRC challenges must stay SILENT while the images they checksum are
// unmodelled. A guessed value cannot be right, and this is the one challenge pair whose
// mismatch arm disconnects: the host recomputes the checksum over its own tables and punts
// on a difference [orig: handle_anti_cheat_crc_check @0x502050], where an unanswered
// challenge costs nothing. Witnessed live 2026-07-26 against a stock retail co-op host —
// a single placeholder C2S 0x20 / 0x21 drew "PUNT ACRC" / "PUNT WCRC" (DC=2, DPC=46) and
// ended the session mid-join, while the same client sending nothing stayed connected.
// This pins the SILENCE so a future "helpful" reply cannot regress joining (D-NET-181).
bool check_crc_challenges_are_not_answered() {
	np::JoinerConnection joiner("ChallengeSilence");
	seed_joiner(joiner);
	SessionSequencing server_tx{1, 0};

	const std::vector<uint8_t> forms[] = {
			frame_s2c(server_tx, {make_protocol_message(0x30, {0x02, 0xEF, 0xBE})}),
			frame_s2c(server_tx, {make_protocol_message(0x30, {0xFF, 0x01, 0x00})}),
			frame_s2c(server_tx, {make_protocol_message(0x30, {0xFF, 0x00, 0x00})}),
			frame_s2c(server_tx, {make_protocol_message(0x31, {0x07, 0x34, 0x12})}),
	};
	for (const std::vector<uint8_t> &challenge : forms) {
		const np::JoinerConnection::PollResult result =
				joiner.handle_datagram(challenge.data(), challenge.size());
		std::vector<uint8_t> body;
		if (!expect(!reply_body(result, 0x20, body) && !reply_body(result, 0x21, body),
		            "an anti-cheat CRC challenge draws no reply while its image is unmodelled"))
			return false;
		// Silence here must not be silence everywhere: the connection is still live and
		// still answers the challenges whose honest values we DO produce.
		if (!expect(!joiner.session_lost(),
		            "and the unanswered challenge does not itself end the session"))
			return false;
	}
	return true;
}

} // namespace

int main() {
	bool ok = true;
	ok = check_captured_punt_decodes_field_for_field() && ok;
	ok = check_decode_is_order_and_shape_robust() && ok;
	ok = check_captured_punt_closes_a_joiner_parked_at_the_deploy_screen() && ok;
	ok = check_ordinary_settings_traffic_is_undisturbed() && ok;
	ok = check_crc_challenges_are_not_answered() && ok;
	return ok ? 0 : 1;
}
