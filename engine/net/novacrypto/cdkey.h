#pragma once

// The Delta Force Xtreme 2 CD key: its three forms, the activation (NWACT) the
// NovaWorld service hands a machine, and the LAN host's join check over it
// (docs/titles/dfx2-vs-jo-re.md section 9, DFX2-13).
//
//   serial    the issuer's plaintext (any bytes; NovaLogic's own format is
//             unwitnessed, the service's is apps/novaworld_web/cd_keys.h's)
//   wire key  the A-P CDKIID a client sends a LAN host and that host decodes:
//             encode_key_chain(serial, kDfx2WireChain)
//   NWACT     the registry CDKIID, bound to one machine: the wire key encoded
//             under kDfx2RegistryChain, then the hardware mix run backwards
//             under the machine's NWPSSK fingerprint
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
// Joint Operations keeps a CDKIID too, but validates nothing with it: its
// registry read only feeds the NovaWorld NWCDKIID form field, which the live
// service accepted empty, and its host has no CD-key leg [orig:
// CDKey_ReadFromRegistryByProduct @ 0x411d30; Server_ValidatePlayerJoinRequest
// @ 0x512100]. Nothing here is JO's.

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::cdkey {

// The DFX2 CD-key descriptor, off_812EE8 (dfx2.exe): six chain pointers. +16 is
// the chain the LAN join check decodes a joiner's CDKIID under (CDKey_DecodeWireJoinChain
// @0x411b40), +20 the chain the registry value decodes under once mixed (CDKey_Decode
// @0x411af0); the other four point at the empty string.
// [orig: off_812EE8 +16 -> @0x7c2620 (dfx2.exe)]
inline constexpr const char *kDfx2WireChain = "5645236346346245:2345346541242546:4563451326353456";
// [orig: off_812EE8 +20 -> @0x7c25e8 (dfx2.exe)]
inline constexpr const char *kDfx2RegistryChain =
		"928975689:234957945234:24598293879654:2349857982794876";
// The CDKIIDEXP1 descriptor, off_812F00 (dfx2.exe), the next six pointers, every one
// at the empty string: a CDKIIDEXP1 decodes under the empty chain, one empty key.
// [orig: off_812F00 +16 -> @0x7bf886 (dfx2.exe)]
inline constexpr const char *kDfx2Exp1Chain = "";

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

// The wire key of `serial`: its CRC-chained A-P encoding under kDfx2WireChain.
std::string make_wire_key(std::string_view serial);
// The serial a wire key holds: false when it does not decode under kDfx2WireChain,
// the LAN join check's code 11 (decode_key_chain's rules: A-P, the keys' CRCs).
bool decode_wire_key(std::string_view wire_key, std::string &serial);

// The NWACT that activates `wire_key` on the machine `fingerprint` names:
// mix_up(encode_key_chain(wire_key, kDfx2RegistryChain), fingerprint), A..Z.
// The same inputs always give the same NWACT.
std::string make_activation(std::string_view wire_key, std::string_view fingerprint);
// The client's reading of an NWACT on its own machine: the mix under its
// fingerprint, then the registry chain decoded, giving the wire key it keeps.
// False where the client's decode fails (its status -3).
// [orig: CDKey_WriteActivationFromNWACT @0x411e30 (dfx2.exe): the fingerprint @0x411ebb,
//  the copy @0x411eca, NapiNP_EncryptStringWithPRNG @0x411ee9, CDKey_Decode under
//  off_812EE8 @0x411f0a, the decoded value stored at +16 @0x411f37]
bool read_activation(std::string_view nwact, std::string_view fingerprint, std::string &wire_key);

// The two CD-key values a player carries: the CDKIID and CDKIIDEXP1 its join
// sends (player +724 / +728), or the host's own (g_CDKeyInfo +16 / +0x810).
struct WireKeys {
	std::string cdkiid;
	std::string exp1;
};

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
// joiner's CDKIID must be present (10) and decode under kDfx2WireChain (11),
// and must equal neither the host's own, when the host has one (12), nor any
// other player's (13); a CDKIIDEXP1, when the joiner sends one, is checked the
// same way under kDfx2Exp1Chain. The comparisons are of the values as sent,
// ASCII case-insensitive (Napi_StrCaseEqual). `others` are the players already
// in the session, the joiner left out.
// [orig: Server_ValidatePlayerJoinRequest @0x50f1d0 (dfx2.exe), the twin of @ 0x512100:
//  the LAN test @0x50f3e2..0x50f3f6, code 10 @0x50f3fc..0x50f407 (@0x50f5b5), the decode
//  @0x50f425 and code 11 @0x50f42e, the host's key @0x50f449..0x50f464 (code 12), the
//  players @0x50f47f..0x50f4c8 (code 13 @0x50f59a), CDKIIDEXP1 @0x50f4ca..0x50f565]
LanJoinVerdict check_lan_join(
		const WireKeys &joiner, const WireKeys &host, const std::vector<WireKeys> &others);

} // namespace opennova::cdkey
