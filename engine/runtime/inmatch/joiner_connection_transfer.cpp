// JoinerConnection - the retained transfer chunks: the S2C 0x45 terrain-load pages staged into
// the .til image, and the S2C 0x60 server-info and 0x64 mission-metadata chunks the session
// keeps (the session variables, the published cap and mpattrib). Split out of
// joiner_connection.cpp; the class header is the shared declaration.
#include <runtime/inmatch/joiner_connection.h>

#include <net/npwire/ingame_decode.h> // FileTransferChunk / decode_terrain_load_batch
#include <net/npwire/session_vars.h>  // decode_session_vars

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace opennova::inmatch {

void JoinerConnection::reset_terrain_load() {
	terrain_til_state_ = TerrainTilState::Absent;
	terrain_til_staging_.clear();
	terrain_tile_count_ = 0;
	terrain_next_index_ = 0;
	terrain_til_bytes_.clear();
}

void JoinerConnection::invalidate_terrain_load() {
	terrain_til_state_ = TerrainTilState::Invalid;
	terrain_til_staging_.clear();
	terrain_tile_count_ = 0;
	terrain_next_index_ = 0;
	terrain_til_bytes_.clear();
}

void JoinerConnection::retain_terrain_load_page(
		const std::vector<uint8_t> &body) {
	if (terrain_til_state_ == TerrainTilState::Invalid) return;
	if (terrain_til_state_ == TerrainTilState::Complete) {
		invalidate_terrain_load();
		return;
	}
	TerrainLoadBatch batch;
	if (!decode_terrain_load_batch(body.data(), body.size(), batch)) {
		invalidate_terrain_load();
		return;
	}

	// The canonical retail writer advances a signed 16-bit cursor. Enforcing its
	// exact contiguous output shape rejects gaps/overlaps that retail's trusting
	// reader would otherwise overwrite, without changing valid-host behavior.
	// [orig: Terrain_SerializeTiles @0x6080F0]
	constexpr uint32_t kMaxTileCount =
			static_cast<uint32_t>(std::numeric_limits<int16_t>::max());
	if (batch.has_header) {
		if (terrain_til_state_ != TerrainTilState::Absent ||
				batch.tile_count == 0 || batch.tile_count > kMaxTileCount ||
				batch.start_index != 0 || batch.end_index == 0 ||
				batch.end_index > batch.tile_count) {
			invalidate_terrain_load();
			return;
		}

		terrain_til_staging_.assign(
				16u + 12u * static_cast<std::size_t>(batch.tile_count), 0u);
		// Preserve all 16 header bytes exactly as sent. In particular hdr2/hdr3
		// are opaque file bytes; reconstructing them from decoded integers would
		// introduce an unnecessary normalization seam.
		std::copy(body.begin() + 4, body.begin() + 20,
				terrain_til_staging_.begin());
		terrain_tile_count_ = batch.tile_count;
		terrain_next_index_ = 0;
		terrain_til_bytes_.clear();
	} else {
		if (terrain_til_state_ != TerrainTilState::Receiving ||
				batch.start_index != terrain_next_index_ ||
				batch.end_index <= batch.start_index ||
				batch.end_index > terrain_tile_count_) {
			invalidate_terrain_load();
			return;
		}
	}

	const std::size_t source_offset = batch.has_header ? 20u : 4u;
	const std::size_t record_count =
			static_cast<std::size_t>(batch.end_index - batch.start_index);
	std::copy(body.begin() + static_cast<std::ptrdiff_t>(source_offset),
			body.begin() + static_cast<std::ptrdiff_t>(
					source_offset + 12u * record_count),
			terrain_til_staging_.begin() + static_cast<std::ptrdiff_t>(
					16u + 12u * batch.start_index));
	terrain_next_index_ = batch.end_index;
	if (terrain_next_index_ == terrain_tile_count_) {
		terrain_til_bytes_ = terrain_til_staging_;
		terrain_til_state_ = TerrainTilState::Complete;
	} else {
		terrain_til_state_ = TerrainTilState::Receiving;
	}
}

void JoinerConnection::retain_server_info_chunk(const FileTransferChunk &chunk) {
	const uint64_t chunk_end = uint64_t(chunk.chunk_offset) + chunk.chunk_size;
	if (chunk.chunk_offset > chunk.total_size || chunk_end > chunk.total_size ||
			chunk.total_size > (1u << 20))
		return;
	if (chunk.transfer_id != server_info_transfer_id_ || chunk.chunk_offset == 0) {
		server_info_transfer_id_ = chunk.transfer_id;
		server_info_bytes_.assign(chunk.total_size, 0);
	}
	if (server_info_bytes_.size() != chunk.total_size) return;
	std::memcpy(server_info_bytes_.data() + chunk.chunk_offset, chunk.chunk_data,
			chunk.chunk_size);
	// The completed stream parses into the session variables
	// [orig: SaveFile_SendAndWaitForServerAck @0x5205d0 ->
	//  Client_ParseServerSessionVariables @0x5202f0].
	if (chunk.is_final())
		decode_session_vars(server_info_bytes_.data(), server_info_bytes_.size(), session_vars_);
}

void JoinerConnection::retain_mission_metadata_chunk(
		const FileTransferChunk &chunk) {
	// The fixed block's dwords at 36 (the published player cap) and 44
	// (mpattrib) ride one retained [36, 48) window, so either dword still
	// assembles when a chunk boundary splits it.
	constexpr uint32_t kFixedWindowOffset = 36;
	constexpr uint32_t kFixedWindowSize = 12;
	constexpr uint16_t kMaxPlayersMask = 0x000Fu; // window bytes 0..3 = offset 36
	constexpr uint16_t kMpAttributesMask = 0x0F00u; // window bytes 8..11 = offset 44
	const uint64_t chunk_end = uint64_t(chunk.chunk_offset) + chunk.chunk_size;
	if (chunk.total_size < kFixedWindowOffset + kFixedWindowSize ||
			chunk.chunk_offset > chunk.total_size ||
			chunk_end > chunk.total_size)
		return;

	const bool new_transfer =
			chunk.transfer_id != mission_metadata_transfer_id_ ||
			chunk.total_size != mission_metadata_total_size_;
	if (new_transfer || chunk.chunk_offset == 0) {
		mission_metadata_transfer_id_ = chunk.transfer_id;
		mission_metadata_total_size_ = chunk.total_size;
		mission_metadata_fixed_bytes_.fill(0);
		mission_metadata_fixed_byte_mask_ = 0;
		mission_metadata_bytes_.assign(chunk.total_size, 0);
	}
	if (mission_metadata_bytes_.size() == chunk.total_size)
		std::memcpy(mission_metadata_bytes_.data() + chunk.chunk_offset, chunk.chunk_data,
				chunk.chunk_size);

	for (uint32_t i = 0; i < kFixedWindowSize; ++i) {
		const uint32_t absolute = kFixedWindowOffset + i;
		if (absolute < chunk.chunk_offset || absolute >= chunk_end) continue;
		mission_metadata_fixed_bytes_[i] =
				chunk.chunk_data[absolute - chunk.chunk_offset];
		mission_metadata_fixed_byte_mask_ |= static_cast<uint16_t>(1u << i);
	}
	const auto window_dword = [&](std::size_t at) {
		return static_cast<uint32_t>(mission_metadata_fixed_bytes_[at]) |
				(static_cast<uint32_t>(mission_metadata_fixed_bytes_[at + 1]) << 8) |
				(static_cast<uint32_t>(mission_metadata_fixed_bytes_[at + 2]) << 16) |
				(static_cast<uint32_t>(mission_metadata_fixed_bytes_[at + 3]) << 24);
	};
	if ((mission_metadata_fixed_byte_mask_ & kMaxPlayersMask) == kMaxPlayersMask)
		session_max_players_ = window_dword(0);
	if ((mission_metadata_fixed_byte_mask_ & kMpAttributesMask) == kMpAttributesMask) {
		mp_attributes_ = window_dword(8);
		++mp_attributes_revision_;
	}
}

} // namespace opennova::inmatch
