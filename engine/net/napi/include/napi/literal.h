#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace opennova {

// Parse one retail NAPI script integer literal. The dispatcher accepts a
// quoted character, hexadecimal ($, 0x, h), octal (leading 0, o), binary
// (%, b), or decimal (optional d), in that order. It intentionally has no
// sign handling; signed configuration fields use the CRT atoi/atol path.
// On success `consumed` identifies the matched prefix and may be shorter than
// `input`, exactly like the original cursor contract.
// [orig: NapiScript_ParseLiteralValue @0x62db00]
bool napi_parse_literal_value(std::string_view input, uint32_t &value,
		size_t *consumed = nullptr);

} // namespace opennova
