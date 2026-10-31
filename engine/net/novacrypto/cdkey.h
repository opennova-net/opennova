#pragma once

// The Delta Force Xtreme 2 CD key: its three forms, the activation (NWACT) the
// NovaWorld service hands a machine, and the LAN host's join check over it
// (docs/titles/dfx2-vs-jo-re.md section 9, DFX2-13).
//
//   serial    the issuer's plaintext (any bytes; NovaLogic's own format is
//             unwitnessed, the service's is apps/novaworld_web/cd_keys.h's)
//   wire key  the A-P CDKIID a client sends a LAN host and that host decodes:
//             encode_key_chain(serial, chains.wire)
//   NWACT     the registry CDKIID, bound to one machine: the wire key encoded
//             under chains.registry, then the hardware mix run backwards
//             under the machine's NWPSSK fingerprint
//
// The chains are the protocol's (Protocol::cd_key, net/protocol/protocol.h: DFX2's,
// games/dfx2/dfx2_protocol.cpp); a protocol with none has no CD key to check.
//
// The client runs it the other way. The NovaWorld screen reads the NWACT
// browser variable (a cookie a web reply sets), writes it to the registry
// value CDKIID as it is, then runs the mix under its own fingerprint and
// decodes the registry chain; what comes out is the wire key it keeps and
// sends [orig: @0x557539..0x55757e (dfx2.exe), in the twin of
// UI_ProcessLANSessionStateMachine @ 0x558de0; CDKey_WriteActivationFromNWACT
// @0x411e30 (dfx2.exe)]. Its boot read does the same over the stored value
// [orig: CDKey_ReadRegistry @0x411b90 (dfx2.exe)]. The serial nests inside the
// wire key, so an activated copy also passes another copy's LAN join check.
//
// Joint Operations keeps a CDKIID too, but validates nothing with it. Its boot
// reads the Typhoon Rising value under that product's own chain, with no mix, and
// a value that decodes feeds two places: the NovaWorld verify Cookie's NWCDKIID,
// which the live service accepted empty, and a LAN JOIN's CDKIID TLV, which a JO
// host stores and never tests (its join check has no CD-key leg). No code of
// Jointops.exe writes the +0x810 CDKIIDEXP1 its JOIN would carry too. The read is
// not ported, so OpenNova's JO sends neither [orig: CDKey_ReadFromRegistryByProduct
// @ 0x411d30, product 0 @ 0x4a7e26..0x4a7e32; CNapiGameSession_OnNovaWorldConnected
// @ 0x4d16e3..0x4d1702; NapiNP_WriteClientAuthPayload @ 0x42a1cd..0x42a235, a LAN
// session's (network type 2 @ 0x42a1c4); Server_ValidatePlayerJoinRequest
// @ 0x512100]. Nothing here is JO's.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::cdkey {

// A protocol's CD-key chains (encode_key_chain's colon-separated keys): the descriptors' chains
// a client and its LAN host decode under.
struct KeyChains {
	// The chain the LAN join check decodes a joiner's CDKIID under, which the wire key is
	// encoded by (CDKey_DecodeWireJoinChain's).
	const char *wire = nullptr;
	// The chain the registry value decodes under once mixed (CDKey_Decode's), the NWACT's.
	const char *registry = nullptr;
	// The chain a CDKIIDEXP1 decodes under.
	const char *exp1 = nullptr;
	// Where the client keeps its activation: the registry key and value its boot read reads and
	// its NWACT is written to (os_registry), and the file that stands in for the value where the
	// platform has no registry (OpenNova's, beside the executable).
	const char *registry_key = nullptr;
	const char *registry_value = nullptr;
	const char *stand_in_file = nullptr;
};

// The NWPSSK fingerprint the hardware mix is keyed by: 23 characters, each 'A',
// 'C' or 'E' plus a nibble of the volume serial, name and file system mix, so
// A..T [orig: CDKey_GenerateHardwareFingerprint @0x4a4a60 (dfx2.exe), the twin of
// @ 0x4a4a00]. The client sets it as the NWPSSK browser variable on connecting to
// NovaWorld, so every web request carries it as a cookie [orig:
// CNapiGameSession_OnNovaWorldConnected @0x4d1914 (dfx2.exe), the twin of @ 0x4d1570].
inline constexpr size_t kFingerprintLength = 23;
// True for a value of that shape: kFingerprintLength upper-case letters.
bool is_fingerprint(std::string_view text);

// The hardware mix over `text` in place, keyed by `fingerprint`: per letter or
// digit one step of the NWU 16-bit LCG, seeded by the fingerprint's key seed,
// and the character shifted DOWN within its class (a-z, A-Z, 0-9) by the step
// modulo the class's size; any other byte is left alone and takes no step. It
// stops at a NUL as the original's string walk does.
// [orig: NapiNP_EncryptStringWithPRNG @ 0x618cc0, NapiNP_EncryptCharWithCipher @ 0x618690];
// [orig: the twins @0x700df0 and @0x7007c0 (dfx2.exe), the same code]
void mix_down(std::string &text, std::string_view fingerprint);
// Its inverse, the issuer's direction: the same steps, each character shifted
// UP. mix_down(mix_up(x)) == x for any x and fingerprint.
void mix_up(std::string &text, std::string_view fingerprint);

// The wire key of `serial`: its CRC-chained A-P encoding under chains.wire.
std::string make_wire_key(const KeyChains &chains, std::string_view serial);
// The serial a wire key holds: false when it does not decode under chains.wire,
// the LAN join check's code 11 (decode_key_chain's rules: A-P, the keys' CRCs).
bool decode_wire_key(const KeyChains &chains, std::string_view wire_key, std::string &serial);

// The NWACT that activates `wire_key` on the machine `fingerprint` names:
// mix_up(encode_key_chain(wire_key + NUL, chains.registry), fingerprint), A..Z,
// 2 * (wire_key.size() + 1 + 4 * the chain's keys) letters. The NUL is part of
// the plaintext because the client's decode writes none: it leaves the plaintext
// in its room followed by the chain's CRC dwords and whatever the room held, and
// its copy to +16 runs to the first NUL, so only the encoded one ends the wire key
// there. The same inputs always give the same NWACT.
// [orig: NapiNP_DecodeEncryptedString @0x701260 (dfx2.exe), in place, no terminator;
//  CDKey_WriteActivationFromNWACT @0x411f37 (dfx2.exe), the Napi_CopyString to +16]
std::string make_activation(
		const KeyChains &chains, std::string_view wire_key, std::string_view fingerprint);
// The wire key an NWACT holds on the machine `fingerprint` names, as the client reads it: the
// value mixed down under the fingerprint, then decoded under chains.registry in its 0x800-byte
// room, the result kept as Napi_CopyString keeps it (to its first NUL, at most 0x7FF
// characters). False, `wire_key` empty, where the decode fails (the client's status -3). The
// client's NovaWorld screen runs it over the NWACT a page hands it, its boot read over the
// registry value that NWACT was stored as; what comes out is the CDKIID its LAN join sends and
// its host compares against (g_CDKeyInfo +16).
// [orig: CDKey_WriteActivationFromNWACT @0x411e30 (dfx2.exe) - the fingerprint @0x411ebb, the
//  copy @0x411eca, NapiNP_EncryptStringWithPRNG @0x411ee9, CDKey_Decode under off_812EE8
//  @0x411f0a (its -3 @0x411f13), the wire key's copy to +16 @0x411f37; CDKey_ReadRegistry
//  @0x411b90 (dfx2.exe) - the same legs @0x411c46..0x411c79, the copy @0x411cd3; CDKey_Decode
//  @0x411af0 (dfx2.exe) - NapiNP_DecodeEncryptedString under the descriptor's +20 chain, room
//  0x800, @0x411b38; Napi_CopyString @ 0x617e10 - at most dst_len - 1 characters, to the first
//  NUL]
bool activation_wire_key(const KeyChains &chains, std::string_view nwact,
		std::string_view fingerprint, std::string &wire_key);

// The two CD-key values a player carries: the CDKIID and CDKIIDEXP1 its join
// sends (player +724 / +728), or the host's own (g_CDKeyInfo +16 / +0x810).
struct WireKeys {
	std::string cdkiid;
	std::string exp1;
};

// The client's CD-key record (g_CDKeyInfo): the status its last read or activation stored (1, or
// -1 for a refused write, -3 for a value that does not decode), the multiplayer flag (a decoded
// value ending "/MP"), and the two ids its LAN join sends (+16 the CDKIID, +0x810 the
// CDKIIDEXP1, which no code of dfx2.exe writes).
// [orig: g_CDKeyInfo @0xa71c38 (dfx2.exe) - +0 the status, +8 g_CDKeyInfoIsMultiplayer @0xa71c40,
//  +16 g_CDKeyInfoWireCdkiid, +0x810 g_CDKeyInfoWireExp1 @0xa72448]
struct KeyInfo {
	int32_t status = 0;
	bool multiplayer = false;
	WireKeys keys;
};
// The process's record, which the boot read fills and an activation updates, and which the LAN
// join, the LAN host's check and the NovaWorld verify Cookie read.
KeyInfo &process_key_info();

// The registry value the record is kept in, as the two operations reach it: `read` gives the
// stored text (false: none could be read), `write` stores one (false: refused, as a write the
// machine key refuses is where no VirtualStore copy takes it, base/os_registry).
struct KeyStore {
	std::function<bool(std::string &)> read;
	std::function<bool(const std::string &)> write;
};
// The platform's store for `chains`: os_registry over its registry key and value, the stand-in
// file `stand_in_dir`/stand_in_file where the platform has no registry.
KeyStore machine_key_store(const KeyChains &chains, const std::string &stand_in_dir);

// The boot read, as the shipped dfx2.exe runs it: the stored value read (once more when the first
// read fails, the second's result ignored), mixed down under this machine's `fingerprint`, decoded
// under chains.registry; the multiplayer flag from a decoded value ending "/MP"; the decoded value
// kept as the CDKIID (activation_wire_key's C string, empty where nothing decodes) and the status
// set to 1, whatever was read. That build carries a patch over the read's three refusals (no
// value, an empty one, one that does not decode), so it returns 1 and its menus' key gate and its
// round end's re-read never refuse; the unpatched branches would store -2 or -3.
// [orig: CDKey_ReadRegistry @0x411b90 (dfx2.exe) - the two reads @0x411bfe / @0x411c21, the
//  patched `jmp` @0x411c2b and NOPs @0x411c3f / @0x411cb9, the fingerprint @0x411c46, the mix
//  @0x411c58, CDKey_Decode @0x411c79 (its -3 @0x411c82), the "/MP" test @0x411c9b..0x411cb2, the
//  status 1 @0x411cbb, the stores @0x411cce / @0x411cd0, the copy to +16 @0x411cd3]
int32_t read_registry(KeyInfo &info, const KeyChains &chains, std::string_view fingerprint,
		const KeyStore &store);

// The activation from an NWACT: the value written to the store (once more when the first write
// is refused); a refused second write stores status -1 and changes nothing else. Then the value is
// mixed down under `fingerprint` and decoded under chains.registry: one that does not decode
// stores -3, one that does stores 1 and its decoded value as the CDKIID. The multiplayer flag is
// left as the boot read set it.
// [orig: CDKey_WriteActivationFromNWACT @0x411e30 (dfx2.exe) - NapiRegistry_WriteStringAtPath
//  @0x411e88 / @0x411e9f, the -1 @0x411eab, the fingerprint @0x411ebb, the copy @0x411eca, the mix
//  @0x411ee9, CDKey_Decode @0x411f0a (the -3 @0x411f13), the 1 @0x411f31 and the copy to +16
//  @0x411f37]
int32_t write_activation(KeyInfo &info, const KeyChains &chains, std::string_view nwact,
		std::string_view fingerprint, const KeyStore &store);

// The DFX2 LAN host's CD-key leg of its join check: Accepted, or the reject
// code the host sends the joiner.
enum class LanJoinVerdict : int {
	Accepted = 0,
	MissingKey = 10, // no CDKIID
	UndecodableKey = 11, // a CDKIID (or a CDKIIDEXP1) that does not decode
	HostsKey = 12, // the host's own CDKIID (or CDKIIDEXP1)
	AnotherPlayersKey = 13, // another player's
};

// The leg as the rewritten check runs it, in a LAN session only (transport 2;
// a NovaWorld session runs none of it), after the app-id check (code 9): the
// joiner's CDKIID must be present (10) and decode under chains.wire (11),
// and must equal neither the host's own, when the host has one (12), nor any
// other player's (13); a CDKIIDEXP1, when the joiner sends one, is checked the
// same way under chains.exp1. The comparisons are of the values as sent,
// ASCII case-insensitive (Napi_StrCaseEqual). `others` are the players already
// in the session, the joiner left out.
// [orig: Server_ValidatePlayerJoinRequest @0x50f1d0 (dfx2.exe), the twin of @ 0x512100:
//  the LAN test @0x50f3e2..0x50f3f6, code 10 @0x50f3fc..0x50f407 (@0x50f5b5), the decode
//  @0x50f425 and code 11 @0x50f42e, the host's key @0x50f449..0x50f464 (code 12), the
//  players @0x50f47f..0x50f4c8 (code 13 @0x50f59a), CDKIIDEXP1 @0x50f4ca..0x50f565]
LanJoinVerdict check_lan_join(const KeyChains &chains, const WireKeys &joiner, const WireKeys &host,
		const std::vector<WireKeys> &others);

} // namespace opennova::cdkey
