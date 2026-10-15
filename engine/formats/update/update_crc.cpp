#include "update_crc.h"

#include <base/io/crc32_mpeg2.h>

namespace opennova::update {

uint32_t command_file_crc(const uint8_t *data, size_t size) {
	// The empty file reads as 0, never as the bare initial value. [orig: CmdCrc_ComputeFile
	// @ 0x4044a0 (UPDATE.EXE)]
	if (size == 0)
		return 0;
	return io::crc32_mpeg2(data, size);
}

uint32_t file_crc(const uint8_t *data, size_t size) {
	return io::novalogic_file_crc(data, size);
}

} // namespace opennova::update
