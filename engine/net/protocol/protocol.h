#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <base/version/version_line.h> // Versions (title_versions)

namespace opennova::cdkey {
struct KeyChains; // net/novacrypto/cdkey.h
} // namespace opennova::cdkey

namespace opennova {

// A protocol (CONTEXT.md "Protocol"): the in-match wire protocol a title speaks and its NovaWorld
// identity, the values its executable compiles in and sends. The JO-family executables run the
// same code over different data (docs/titles/dfx2-vs-jo-re.md DFX2-3, "same code, different
// data"), so a Protocol holds that data and every consumer stays the one port over it. Each
// title's lives in games/<code>/<code>_protocol.cpp, which games::install() registers under the
// title's code (install_protocols); several titles may speak one Protocol object (OP: Acid
// Gambit speaks JO's).
//
// A field no binary or service page of the protocol has been read for is UNWITNESSED: nullptr or
// std::nullopt, never another protocol's value. A consumer speaks only the parts it uses complete
// (ProtocolPart): a protocol with an unwitnessed field there is refused by name, never padded
// with JO's.

// The build-environment values a joiner's 0x42 ClientAuth carries as CU tags and a host's join
// check requires, each failing with its own code: BN (2), VN (3), MBN (4), SOPD (8). The joiner
// sends the values its own executable compiles in; the host compares against its own.
struct JoinBuild {
	int32_t bn = 0;
	int32_t vn = 0;
	int32_t mbn = 0;
	int32_t sopd = 0;
};

// A generation of NovaLogic's network code (CONTEXT.md "Generation"): what every executable of it
// compiles in whatever its title, its NAPI version (NVS) and company (CO), the protocol revision
// a host checks (PV2: the protocol version) and the 0x42's build values. The shared 2009
// generation is games/novalogic_2009's.
struct Generation {
	const char *nvs = nullptr;
	const char *co = nullptr;
	const char *pv2 = nullptr;
	std::optional<JoinBuild> join_build;
};

// The in-match code's per-protocol branches: the host and joiner code the JO-family executables
// share, edited at a few constants and tests (docs/titles/dfx2-vs-jo-re.md §7, the §8 catalog).
// Each field names its catalog row. The defaults are JO's, the row every other title is read
// against.
struct ProtocolInMatch {
	// DFX2-1: S2C 0x0A's objective sub-block (g_GameType & 0x20000) carries a fifth u32 after the
	// four objective values, the co-op checkpoint, which the joiner reads.
	bool objective_checkpoint = false;
	// DFX2-2: S2C 0x0F's flags bit 0 is set whether or not the mission has spawn zones.
	bool spawn_zone_flag_always = false;
	// DFX2-10: the weapon availability value that means "armory only" (JO 2, DFX2 3); the other of
	// 2 and 3 is the mission's default. S2C 0x66 carries the 0 entries and this value's.
	int32_t armory_only_availability = 2;
	// DFX2-11: the join writes the slot state byte 0x13 (undeployed, with the pre-round loadout
	// latch) whatever the mission and the pre-round timer, and every deploy writes 3 (the latch).
	bool join_state_fixed = false;
	// DFX2-12: the joiner's C2S 0x2F class byte is 8 (rifleman), or 7 (gunner) in a mission of
	// type 2, whatever the profile's class.
	bool loadout_class_fixed = false;
	// DFX2-14: the relayed chat wraps the sender's squad tag in <ch> and <co>.
	bool chat_squad_tag_wrapped = false;
	// DFX2-18: the bits the mission-data block's rules word (+44) is ORed with.
	uint32_t rules_word_bits = 0;
	// D-SIM-52: the people the S2C person records can name, pool 0's cap however a mount scales
	// it (world::wire_pool_capacities). Each names a person by the one-byte pool-0 index
	// (pool0_index_byte, runtime/world/entity.h): the 0x1E, 0x2D, 0x6D and 0x33 records, and
	// 0x46's entity byte. 256 in JO's and DFX2's.
	uint32_t pool0_wire_capacity = 256;
};

struct Protocol {
	// OpenNova's short name for the protocol ("jo", "dfx2"), and its label ("JO", shown as
	// "JO protocol 16").
	const char *code = nullptr;
	const char *name = nullptr;

	// --- The in-match game protocol: the identity block CNapiNetwork_Init installs, which the
	// 0x41 hello, the 0x42 join and the host's 0x81 carry and the host's gate compares (NVS, PN,
	// PG and PV1 on both; PV2 on the 0x42). NVS, CO and PV2 are its generation's. No AP: the
	// protocol's is the running executable's file name, whatever the title
	// (make_protocol_client_hello, D-NET-975).
	const char *pn = nullptr;
	std::optional<std::array<uint8_t, 16>> guid; // PG, the protocol GUID's 16 bytes
	const char *pv1 = nullptr;
	// PV3's bracket pair: the ADDRESSES of two string literals of the image (the executable's own
	// data layout, not a build number). PV3's version quad and date are no protocol's: they are
	// the engine build's, ONE's (protocol_version3, net/npwire/session_hello.h).
	std::array<uint32_t, 2> pv3_tags{};
	const Generation *generation = nullptr;
	// The in-match code's branches (witnessed per protocol: one without them is refused by its
	// session part, never read with JO's).
	std::optional<ProtocolInMatch> in_match;
	// The CD key the protocol's clients carry, its chains (net/novacrypto/cdkey.h): DFX2-13, a LAN
	// host's join check tests a joiner's CDKIID and CDKIIDEXP1 under them (codes 10..13,
	// cdkey::check_lan_join), and the service's activation (NWACT) encodes under them. Null for
	// none: JO's CDKIID validates nothing (cdkey.h).
	const cdkey::KeyChains *cd_key = nullptr;

	// --- NovaWorld: the host list's version pair (and the start page's [VER1] / [VER2]), the
	// start page's [PRODUCTCODE], the gate manager's default tag and gt.ssc's key chain. The web
	// flow's pages, its pfid among their links, are the service's: the gate's start page and the
	// pages it leads to name them, the lobby page's template its list and its join
	// (LobbyHttpFlow::gsb_request, join).
	const char *ver1 = nullptr;
	const char *ver2 = nullptr;
	const char *product_code = nullptr;
	const char *gate_tag = nullptr;
	const char *gate_tag_keys = nullptr;
	// Whether the host lists carry DFXCATEGORY1 after LAN (HostRegistration::category_var), a
	// witnessed difference rather than an unwitnessed value: absent from JO's lists, present in
	// DFX2's (docs/titles/dfx2-vs-jo-re.md DFX2-7).
	bool host_category_var = false;
	// The sign-in page's submit control (its template's form-post button): the one the
	// executable's LOGIN callbacks and its login validation name when it is "LOGIN"
	// (LobbyHttpFlow::sign_in_submit_enabled, D-NET-1335).
	const char *sign_in_submit = nullptr;

	// The executable's engine build is no part of a protocol: NovaLogic's 1.7.5.7, the same in
	// JO and DFX2 and compared by no peer, sits in slots ONE's build fills (one_build(): the
	// 0x41 / 0x42 / 0x81 BDAT and PV3, the lobby connection's BDAT and its 0x42 join's
	// BuildDateAndTime, the joiner's VERSIONSTRING and 0x0B dword, the host lists' GV, the
	// METPROTOCOL reports' GV and the connect log's VERSION). Each slot's writer cites the
	// retail value it replaces.
};

// The rows games::install() registers: each title's code and the protocol it speaks.
struct TitleProtocol {
	const char *title_code = nullptr;
	const Protocol *protocol = nullptr;
};

// Fills the registry with `rows` (each protocol outliving the program); a second call replaces
// the first. Every lookup below aborts on an EMPTY registry, naming the invariant: a program that
// reaches them calls games::install() at its start (games/games.h).
void install_protocols(const TitleProtocol *rows, size_t count);

// The protocol title `title_code` speaks (compared case-insensitively); null for an unknown title.
const Protocol *title_protocol(std::string_view title_code);
// The protocol whose code is `code` (case-insensitively); null for none.
const Protocol *protocol_by_code(std::string_view code);
// The protocol whose in-match protocol name is `pn`, compared case-insensitively as the host's
// gate compares it [orig: Napi_StrCaseEqual @0x616e70, used by NapiNPProtocol_HandleClientJoin
// @0x62B750]; null when no protocol's PN is that (or witnessed). Titles that speak one Protocol
// object resolve to it once.
const Protocol *protocol_by_pn(std::string_view pn);
// The protocol version (CONTEXT.md "Protocol version"): PV2, its generation's; null unwitnessed.
const char *protocol_version(const Protocol &protocol);
// The title's parts of a version line (base/version/version_line.h) for a launch that runs as
// title `title_code`: the title's display name where `versions` names no game yet, and the
// protocol the title speaks with its protocol version (PV2), "JO" and "16", where that protocol's
// session part is witnessed in full. An unknown title adds nothing; one whose protocol is
// unwitnessed (DFX's, BHD's) leaves the protocol part out.
void title_versions(std::string_view title_code, Versions &versions);

// The parts of a protocol, each complete or not on its own: what the in-match protocol and the
// NovaWorld session speak (the hello block, the version pair, the product code, the gate tag and
// its key chain), and what the NovaWorld web flow's client acts on of its own (the sign-in page's
// submit control).
enum class ProtocolPart : uint8_t { Session, WebFlow };

// The names of the fields of `part` that `protocol` leaves unwitnessed, in the row's order
// ("NVS", "CO", "PN", "PG", "PV3", ..., or "sign-in submit"); empty for a complete part.
std::vector<std::string> protocol_missing(const Protocol &protocol, ProtocolPart part);
bool protocol_complete(const Protocol &protocol, ProtocolPart part);
// "the <code> title's <part> identity is not witnessed yet: <fields>" for a part `protocol` leaves
// incomplete, empty for a complete one.
std::string protocol_refusal(const Protocol &protocol, ProtocolPart part);
// The protocol a title code (the `/game <code>` flag) speaks: the title's, when its session part
// is witnessed in full. Null otherwise, with `error` naming the code that is unknown or the
// fields the protocol leaves unwitnessed. A consumer of the web flow checks that part itself
// (protocol_missing(.., WebFlow)).
const Protocol *resolve_title_protocol(std::string_view title_code, std::string &error);

// The in-match branches a session's protocol takes, and its 0x42 build values. Every host and
// joiner runs its title's protocol, set before its first in-match read: a read with none, or with
// a protocol that leaves the part unwitnessed, aborts naming the invariant. No session runs a
// default title's.
const ProtocolInMatch &protocol_in_match(const Protocol *protocol);
const JoinBuild &protocol_join_build(const Protocol *protocol);

} // namespace opennova
