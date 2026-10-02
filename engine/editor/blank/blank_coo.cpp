#include "blank_makers.h"

namespace opennova::editor {

// nw_cdata.coo is the menu shell's cached NovaWorld data table: a 4-byte header the
// loader discards, then the "RSTR" magic and records whose strings are encrypted with
// a per-machine volume fingerprint [orig: CUIStringTable_OpenAndLoad @ 0x63a500 reads
// and drops 4 bytes; CUIStringTable_LoadFromFile @ 0x64f290 checks the magic, then
// decrypts every record string with CDKey_GenerateVolumeFingerprint's key]. A record
// written on one machine is unreadable on another, so the only portable file is the
// empty table: header + magic, no records. The loader accepts it (fewer than four
// bytes after the header returns success; the magic with no records too).
bool make_blank_coo(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &) {
	out = { 0, 0, 0, 0, 'R', 'S', 'T', 'R' };
	return true;
}

} // namespace opennova::editor
