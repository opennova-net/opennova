#include <net/protocol/protocol.h>

#include <base/io/strutil.h>
#include <base/title/title.h>

#include <cstdio>
#include <cstdlib>

namespace opennova {

namespace {

std::vector<TitleProtocol> &rows() {
	static std::vector<TitleProtocol> table;
	return table;
}

// Every lookup's precondition: the program installed its protocols.
const std::vector<TitleProtocol> &installed() {
	const std::vector<TitleProtocol> &table = rows();
	if (table.empty()) {
		std::fputs("no protocols installed: the program must call games::install()\n", stderr);
		std::abort();
	}
	return table;
}

void missing_if(std::vector<std::string> &out, bool unwitnessed, const char *name) {
	if (unwitnessed)
		out.emplace_back(name);
}

const char *part_name(ProtocolPart part) {
	return part == ProtocolPart::WebFlow ? "web flow" : "session";
}

// The protocol the default title speaks: what a session brought up with none runs.
const Protocol *default_protocol() {
	return title_protocol(default_title().code);
}

} // namespace

void install_protocols(const TitleProtocol *table, size_t count) {
	rows().assign(table, table + count);
}

const Protocol *title_protocol(std::string_view title_code) {
	for (const TitleProtocol &row : installed())
		if (strutil::iequals(row.title_code, title_code))
			return row.protocol;
	return nullptr;
}

const Protocol *protocol_by_code(std::string_view code) {
	for (const TitleProtocol &row : installed())
		if (row.protocol->code != nullptr && strutil::iequals(row.protocol->code, code))
			return row.protocol;
	return nullptr;
}

const Protocol *protocol_by_pn(std::string_view pn) {
	// The first row naming the PN: titles that speak one object (OP: Acid Gambit, JO) resolve to it
	// alike, so the walk compares protocols, never titles. The PN compares without case, as the
	// host's gate compares it [orig: Napi_StrCaseEqual @0x616e70, used by
	// NapiNPProtocol_HandleClientJoin @0x62B750].
	for (const TitleProtocol &row : installed())
		if (row.protocol->pn != nullptr && strutil::iequals(pn, row.protocol->pn))
			return row.protocol;
	return nullptr;
}

const char *protocol_version(const Protocol &protocol) {
	return protocol.generation != nullptr ? protocol.generation->pv2 : nullptr;
}

void title_versions(std::string_view title_code, Versions &versions) {
	const Title *title = title_code.empty() ? nullptr : title_by_code(title_code);
	if (title == nullptr)
		return;
	if (versions.game.empty() && title->display_name != nullptr)
		versions.game = title->display_name;
	const Protocol *protocol = title_protocol(title_code);
	if (protocol == nullptr || protocol->name == nullptr ||
			protocol_version(*protocol) == nullptr ||
			!protocol_complete(*protocol, ProtocolPart::Session))
		return;
	versions.protocol = protocol->name;
	versions.protocol_version = protocol_version(*protocol);
}

std::vector<std::string> protocol_missing(const Protocol &p, ProtocolPart part) {
	std::vector<std::string> out;
	if (part == ProtocolPart::WebFlow) {
		missing_if(out, p.sign_in_submit == nullptr, "sign-in submit");
		return out;
	}
	const Generation *g = p.generation;
	missing_if(out, g == nullptr || g->nvs == nullptr, "NVS");
	missing_if(out, g == nullptr || g->co == nullptr, "CO");
	missing_if(out, p.pn == nullptr, "PN");
	missing_if(out, !p.guid.has_value(), "PG");
	missing_if(out, p.pv1 == nullptr, "PV1");
	missing_if(out, g == nullptr || g->pv2 == nullptr, "PV2");
	missing_if(out, p.pv3_tags == std::array<uint32_t, 2>{}, "PV3");
	missing_if(out, g == nullptr || !g->join_build.has_value(), "join build values");
	missing_if(out, !p.in_match.has_value(), "in-match branches");
	missing_if(out, p.ver1 == nullptr, "Ver1");
	missing_if(out, p.ver2 == nullptr, "Ver2");
	missing_if(out, p.product_code == nullptr, "product code");
	missing_if(out, p.gate_tag == nullptr, "gate tag");
	missing_if(out, p.gate_tag_keys == nullptr, "gt.ssc key chain");
	return out;
}

bool protocol_complete(const Protocol &protocol, ProtocolPart part) {
	return protocol_missing(protocol, part).empty();
}

std::string protocol_refusal(const Protocol &protocol, ProtocolPart part) {
	const std::vector<std::string> missing = protocol_missing(protocol, part);
	if (missing.empty())
		return std::string();
	std::string out = "the " + std::string(protocol.code != nullptr ? protocol.code : "?") +
			" title's " + part_name(part) + " identity is not witnessed yet: ";
	for (std::size_t i = 0; i < missing.size(); ++i)
		out += (i == 0 ? "" : ", ") + missing[i];
	return out;
}

const ProtocolInMatch &protocol_in_match(const Protocol *protocol) {
	if (protocol != nullptr && protocol->in_match.has_value())
		return *protocol->in_match;
	const Protocol *fallback = default_protocol();
	if (fallback != nullptr && fallback->in_match.has_value())
		return *fallback->in_match;
	static const ProtocolInMatch none{};
	return none;
}

const JoinBuild &protocol_join_build(const Protocol *protocol) {
	for (const Protocol *p : { protocol, default_protocol() })
		if (p != nullptr && p->generation != nullptr && p->generation->join_build.has_value())
			return *p->generation->join_build;
	static const JoinBuild none{};
	return none;
}

gameprofile::GameId protocol_game_id(const Protocol *protocol) {
	for (size_t i = 0; protocol != nullptr && i < title_count(); ++i)
		if (title_protocol(title_at(i)->code) == protocol)
			return title_game_id(*title_at(i));
	return gameprofile::GAME_JO;
}

const Protocol *game_id_protocol(int game_id) {
	for (size_t i = 0; i < title_count(); ++i)
		if (title_game_id(*title_at(i)) == game_id)
			return title_protocol(title_at(i)->code);
	return nullptr;
}

const Protocol *resolve_title_protocol(std::string_view title_code, std::string &error) {
	const Protocol *protocol = title_protocol(title_code);
	if (protocol == nullptr) {
		error = "unknown game '" + std::string(title_code) + "'";
		return nullptr;
	}
	const std::string refusal = protocol_refusal(*protocol, ProtocolPart::Session);
	if (!refusal.empty()) {
		error = refusal;
		return nullptr;
	}
	return protocol;
}

} // namespace opennova
