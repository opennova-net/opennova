#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova {

// NAPI TLV container — the inner message format carried by the envelope.
//
// Witnessed byte-exact in the demo image
// [orig: jodemo NapiMessage_Serialize @0x5f68b0 (container writer); jodemo NapiField_Serialize @0x5f6560 (field writer);
//  jodemo sub_5F6700 @0x5f6700 (container reader); jodemo sub_5F6620 @0x5f6620 (field reader); jodemo sub_5F6270 @0x5f6270 (install field)]
// and in retail Jointops.exe as the gate STATEMENT codec (docs/net/novaworld-net-re.md):
// [orig: NapiStatement_Serialize @0x632870 (container writer); NapiStatementParam_Serialize @0x6325d0 (field writer);
//  NapiStatementContext_ParseNode @0x632fc0 (container reader: 0x02 name, child 0x02 / param 0x04 / end 0x03);
//  NapiStatementParam_ParseFromBuffer @0x632e30 (field reader: 0x04 name LE16 len data 0x00 0x05)]
//
// Wire format (all integers little-endian):
//
//   Container:
//     [0x02] <name bytes> 0x00 [fields...] [children...] [0x03]
//
//   Field (carried inside a container; may be preceded or followed by
//   sub-containers in the wire, but the serializer always emits all fields
//   first then all children):
//     [0x04] <name bytes> 0x00 <LE16 data length> <data bytes> 0x00 [0x05]
//
// Overheads:
//   Container: 1 (0x02) + name.size() + 1 (name NUL) + payload + 1 (0x03)
//              == name.size() + 3  + sum(child sizes)
//   Field:     1 (0x04) + name.size() + 1 (name NUL) + 2 (len) + data.size()
//              + 1 (trailing NUL after data) + 1 (0x05)
//              == name.size() + data.size() + 6
//
// Bounds (both directions — the reader runs the same constructors, so a
// stock peer rejects the whole statement stream on a violation): container
// and field names are 1..63 bytes, field data is under 4096 bytes.
// [orig: NapiStatement_Create @0x632990 @0x6329c3; NapiStatementParam_Create
//  @0x632b30 @0x632b71 / @0x632b90; ParseFromBuffer -> Create -> -14]

struct NapiField {
	std::string name;
	std::vector<uint8_t> data;
};

// The wire's NUL-padded string fields, decoded: trailing NULs are padding,
// never content (the one primitive behind the per-lib field_to_string /
// strip_nul copies this replaced).
inline std::string field_to_string(const uint8_t *data, size_t len) {
	while (len > 0 && data[len - 1] == 0) --len;
	return std::string(reinterpret_cast<const char *>(data), len);
}

inline std::string field_to_string(const NapiField &f) {
	return f.data.empty() ? std::string()
	                      : field_to_string(f.data.data(), f.data.size());
}

struct NapiMessage {
	std::string name;
	std::vector<NapiField> fields;
	std::vector<NapiMessage> children;
};

// Compute the exact serialized size of a message tree, in bytes.
size_t napi_message_size(const NapiMessage &msg);

// Serialize a message tree into `out[0..out_size)`. Returns 0 on success
// or -1 on invalid input or insufficient capacity.
int napi_message_encode(const NapiMessage &msg, uint8_t *out, size_t out_cap, size_t *out_size);

// Parse a single message starting at `data[0]`. On success, populates `out`
// and writes the number of consumed bytes to `bytes_consumed`. Returns 0
// on success, -1 on malformed input. The parser accepts fields and
// children interleaved in any order (peeks the next marker byte to
// decide), matching the original parser's tolerance.
int napi_message_decode(const uint8_t *data, size_t data_len, NapiMessage &out, size_t *bytes_consumed);

// ---------------------------------------------------------------------------
// Message-stream layer (what lives inside the LSB-scatter CRC envelope).
//
// Witnessed byte-exact in the demo image [orig: jodemo CBufferList_Serialize @0x5f69e0 (encoder); jodemo sub_5F6810 @0x5f6810 (decoder)]
// and in retail [orig: NapiStatementContext_SerializeToBuffer @0x6331d0 (encoder, appends the lone 0x01);
//  NapiStatementContext_DeserializeFromBuffer @0x633510 (decoder, 0x01 end marker)]. Stream format:
//
//   <container1 bytes> <container2 bytes> ... <containerN bytes> [0x01]
//
// I.e. a run of 0x02-framed containers, terminated by a lone 0x01 byte.
// There is NO flags-byte bitfield / LEN8/LEN16 / SEQ8/SEQ16 header at
// this layer — the doc T§2.2 description is refuted by IDA (the putative
// flags bitfield came from Wireshark-dissector analysis; the binary does
// a simple type-byte dispatch: 0x01 = end, 0x02 = container, else =
// error). See docs/net/novaworld-net-re.md (the NapiMessage TLV framing).
//
// Overhead: +1 byte for the trailing 0x01 terminator.

// Compute serialized size of a stream.
size_t napi_stream_size(const std::vector<NapiMessage> &messages);

// Serialize a sequence of messages plus the 0x01 terminator. Returns 0 on
// success or -1 if `out_cap` can't hold the full stream.
int napi_stream_encode(const std::vector<NapiMessage> &messages,
                       uint8_t *out, size_t out_cap, size_t *out_size);

// Parse a sequence of messages. Stops at the 0x01 terminator, end-of-buffer,
// or error. Returns 0 on success. `bytes_consumed` reports how many bytes
// (including the 0x01 terminator, if present) were absorbed.
int napi_stream_decode(const uint8_t *data, size_t data_len,
                       std::vector<NapiMessage> &out, size_t *bytes_consumed);

} // namespace opennova
