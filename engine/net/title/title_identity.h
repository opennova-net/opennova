#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace opennova {

class ResourceIndex;

// A title's wire and NovaWorld identity (CONTEXT.md "Title"): the values its executable compiles
// in and sends, one row per gameprofile::GameId. The JO-family executables run the same code over
// different data (docs/titles/dfx2-vs-jo-re.md DFX2-3, "same code, different data"), so a row
// holds that data and every consumer stays the one port over it. The table lives here, in the wire
// group, because its values are wire bytes; engine/base/gameprofile stays the dependency-free
// source of the game ids and codes.
//
// A field no binary or service page of the title has been read for is UNWITNESSED: nullptr or
// std::nullopt, never another title's value. A consumer speaks only the identity parts it uses
// complete (TitleIdentityPart): a title with an unwitnessed field there is refused by name,
// never padded with JO's.

// The in-match protocol's PV3 build string inputs: the version quad and the ADDRESSES of two string
// literals of the image (the bracket pair is the executable's own data layout, not a build number),
// then the build's __DATE__ " " __TIME__ text (title_protocol_version3 assembles them).
// [orig: CNapiNetwork_Init @0x4ca4a0 - the sprintf @0x4ca83d..0x4ca85b over "%ld.%ld.%ld.%ld
//  [%ld.%ld]" @0x7ca680; the date @0x4ca894]
struct TitleBuild {
	std::array<uint32_t, 4> version{};
	uint32_t tag_address_a = 0;
	uint32_t tag_address_b = 0;
	const char *date = nullptr;
};

// The NovaWorld web flow's page names. Retail's client renders the pages the gate's start page
// leads to (the NWStart query, the login form's hidden fields, the NWJoin link, the server-list
// feed); LobbyHttpFlow requests them by these names (docs/net/novaworld-net-re.md D-NET-31: the
// names are the pages' markup, not C literals). A null name is one the title's pages do not
// carry: the request leaves its field out.
struct TitlePages {
	const char *msgbase = nullptr;           // MSGBASE / msgbase
	const char *main = nullptr;              // IN, the login's success, NWJoin's failure
	const char *login = nullptr;             // OUT, the login's failure
	const char *version_file = nullptr;      // verfile
	const char *new_update = nullptr;        // newupdateavailable
	const char *new_update_bypass = nullptr; // newupdateavailablewithbypass
	const char *junction = nullptr;          // junction
	const char *activate = nullptr;          // ACT, between OUT and verfile
	const char *need_to_agree = nullptr;     // needtoagree
	const char *no_db = nullptr;             // nodb
	const char *relay = nullptr;             // relay
	const char *enter_key = nullptr;         // enterkey
	// The login form's charglsvssrequest, the first of its hidden fields as the client posts
	// them (the reverse of the page's order, as JO's captured post shows its own).
	const char *glsvss_request = nullptr;
	const char *join_lan = nullptr;          // NWJoin's leading lan
	const char *key_error = nullptr;         // NWJoin's needexpkey
	const char *join = nullptr;              // NWJoin's success, the .joi
	const char *server_list = nullptr;       // the GSB feed ("<name>?a=1")
};

struct TitleIdentity {
	int game_id = -1; // gameprofile::GameId

	// --- The in-match game protocol: the identity block CNapiNetwork_Init installs, which the
	// 0x41 hello, the 0x42 join and the host's 0x81 carry and the host's gate compares (NVS, PN,
	// PG and PV1 on both; PV2 on the 0x42).
	const char *nvs = nullptr;
	const char *co = nullptr;
	const char *ap = nullptr;
	const char *bdat = nullptr;
	const char *pn = nullptr;
	std::optional<std::array<uint8_t, 16>> guid; // PG, the protocol GUID's 16 bytes
	const char *pv1 = nullptr;
	const char *pv2 = nullptr;
	std::optional<TitleBuild> build; // PV3

	// --- The NovaWorld session (NOVAWORLDUDP) connection CNapiGameSession_InitNPConnection makes.
	const char *lobby_pv1 = nullptr;
	const char *lobby_bdat = nullptr; // its BDAT and the 0x42 join's BuildDateAndTime CU

	// --- NovaWorld: the host list's version pair (and the start page's [VER1] / [VER2]), the
	// start page's [PRODUCTCODE], the gate manager's default tag and gt.ssc's key chain, the
	// login's pfid and the web flow's pages.
	const char *ver1 = nullptr;
	const char *ver2 = nullptr;
	const char *product_code = nullptr;
	const char *gate_tag = nullptr;
	const char *gate_tag_keys = nullptr;
	// Whether the host lists carry DFXCATEGORY1 after LAN (HostRegistration::category_var), a
	// witnessed difference rather than an unwitnessed value: absent from JO's lists, present in
	// DFX2's (docs/titles/dfx2-vs-jo-re.md DFX2-7).
	bool host_category_var = false;
	std::optional<int> pfid;
	std::optional<TitlePages> pages;
};

// The parts of an identity, each complete or not on its own: what the in-match protocol and the
// NovaWorld session speak (the hello block, the lobby's PV1 and BDAT, the version pair, the
// product code, the gate tag and its key chain, the version text), and what the NovaWorld web
// flow requests by name (the pfid and the pages, which no executable carries: the gate's start
// page and the service's pages name them).
enum class TitleIdentityPart : uint8_t { Session, WebFlow };

// The row for a GameId (gameprofile has a row for each); null for an unknown id.
const TitleIdentity *title_identity(int game_id);
// The row whose in-match protocol name is `pn`, compared case-insensitively as the host's gate
// compares it [orig: Napi_StrCaseEqual @0x616e70, used by NapiNPProtocol_HandleClientJoin
// @0x62B750]; null when no row's PN is that (or witnessed).
const TitleIdentity *title_identity_by_protocol_name(std::string_view pn);
// The names of the fields of `part` that `title` leaves unwitnessed, in the row's order ("NVS",
// "PG", "PV3", ..., or "pfid", "web pages"), the session's after the game profile's version text
// (GameProfile::version_text, which the joiner's 0x42 carries) when that is unwitnessed; empty for
// a complete part.
std::vector<std::string> title_identity_missing(const TitleIdentity &title, TitleIdentityPart part);
bool title_identity_complete(const TitleIdentity &title, TitleIdentityPart part);
// The identity a game code (the `/game <code>` flag, gameprofile's codes) speaks: the code's row,
// when its session part is witnessed in full. Null otherwise, with `error` naming the code that
// is unknown or the fields the title leaves unwitnessed. A consumer of the web flow checks that
// part itself (title_identity_missing(.., WebFlow)).
const TitleIdentity *resolve_title_identity(std::string_view game_code, std::string &error);
// "the <code> title's <part> identity is not witnessed yet: <fields>" for a part `title` leaves
// incomplete, empty for a complete one.
std::string title_identity_refusal(const TitleIdentity &title, TitleIdentityPart part);

// ---- the gate tag ----------------------------------------------------------------------------

// gt.ssc's size bound, and the decoder's room: a file larger than 0x1FFF bytes is not read, and
// one whose A-P text decodes to more than 2047 bytes does not decode.
// [orig: Mission_LoadEncryptedConfig @0x4cdcd0 - `file_size <= 0x1FFF` @0x4cdd20, the 2047-byte
//  room @0x4cdd65 -> NapiNP_DecodeEncryptedString @0x619130 (bufSize @0x61920f)]
inline constexpr std::size_t kGateTagFileMaxBytes = 0x1FFF;
inline constexpr std::size_t kGateTagDecodeRoom = 2047;
// The gate tag's own buffer: Napi_CopyString into the gate manager's 64-byte tag keeps 63
// characters. [orig: Mission_LoadEncryptedConfig @0x4cdd7e; CNapiGateManager +76]
inline constexpr std::size_t kGateTagChars = 63;

// gt.ssc's bytes as the boot reads them: the text up to its first NUL (the read buffer is
// NUL-terminated at the file's size), decoded under `key_chain` (pubcrypto.h decode_key_chain),
// and the plaintext up to its first NUL kept at kGateTagChars characters. False, `tag` untouched,
// for a file over kGateTagFileMaxBytes or text that does not decode; an empty plaintext decodes
// to an empty tag, as retail's copy makes one.
// [orig: Mission_LoadEncryptedConfig @0x4cdcd0 - the read @0x4cdd32, the NUL @0x4cdd53, the
//  decode @0x4cdd65 (skipped below zero), its NUL @0x4cdd79, Napi_CopyString(+76, .., 64)
//  @0x4cdd7e]
bool decode_gate_tag_file(const std::vector<uint8_t> &bytes, const char *key_chain,
		std::string &tag);

// The gate tag a title probes the gate with and its NovaWorld session carries (the 0x42's NA and
// GateTag CU, the start page's [GT]): the gate manager's default, the title's, replaced at boot by
// the mount's gt.ssc when one is there and decodes under the title's key chain. The boot reads
// gt.ssc loose first, whatever the session's lookup order: the expansion's folder, the game's
// folder, then the mounted archives (Vfs's ForceLooseFirst). `mount` null reads none. `title`
// must be complete.
// [orig: CNapiGateManager_InitDefaults @0x4d1460 (Napi_CopyString(this+76, "jop:cus2", 64)
//  @0x4d14b6); Game_InitSubsystems @0x4a7149..0x4a714e -> Mission_LoadEncryptedConfig
//  @0x4cdcd0 over g_GateManager - FileSystem_SetSearchLooseFirst(1) @0x4cdcf4,
//  FileSystem_FileExists("gt.ssc") @0x4cdcfe]
std::string title_gate_tag(const TitleIdentity &title, const ResourceIndex *mount);

} // namespace opennova
