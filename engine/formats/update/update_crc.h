#pragma once

// The updater's checksums, as UPDATE.EXE 2.1.4.2 computes and compares them
// (docs/update/updater-re.md §10). Both are CRC-32 over polynomial 0x04C11DB7, MSB first, initial
// 0xFFFFFFFF; they differ in the final complement and in one retail defect.

#include <cstddef>
#include <cstdint>

namespace opennova::update {

// The command file's CRC, compared with the server's CRC reply: CRC-32/MPEG-2 (no final
// complement) over the whole file, and 0 for an empty one.
// [orig: CmdCrc_ComputeFile @ 0x4044a0 (UPDATE.EXE)]
uint32_t command_file_crc(const uint8_t *data, size_t size);

// A command row's crc: the file CRC with the retail unroll defect (io::novalogic_file_crc). The
// precheck compares it with a loose file's CRC and with a PFF entry's checksum word, which the
// patch-era pack.exe stamps with the same CRC, so a Loose and a Copy row both carry it.
// [orig: FileCrc_ComputeFile @ 0x404ac0 (UPDATE.EXE); Cupdate2Dlg_PrecheckCommands @ 0x407820
// (UPDATE.EXE)]
uint32_t file_crc(const uint8_t *data, size_t size);

// A CRC the way the wire and the command file carry it: a signed decimal, since the client
// reads every number with atol. [orig: NLUpdateProtocol_ParseReplyHeader @ 0x4011d0
// (UPDATE.EXE); CmdFile_ReadCommandRow @ 0x404f60 (UPDATE.EXE)]
inline int32_t crc_as_signed(uint32_t crc) {
	return static_cast<int32_t>(crc);
}

} // namespace opennova::update
