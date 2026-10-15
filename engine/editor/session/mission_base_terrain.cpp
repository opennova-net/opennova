#include <editor/session/mission_base_terrain.h>

#include <base/io/crt_ftol.h>
#include <base/vfs/file_source.h>
#include <formats/cpt/cpt_io.h>
#include <formats/trn/trn_io.h>

namespace opennova::editor {

bool read_base_trn(const FileSource &files, const std::string &trn_file, TrnConfig &trn, int32_t &scale) {
	std::vector<uint8_t> bytes;
	std::string error;
	if (!files.read(trn_file, bytes)) return false;
	const std::string text(bytes.begin(), bytes.end());
	if (!load_mission_trn(text, TrnLaterTexts{}, trn, error)) return false;
	scale = 128;
	for (const TrnKeyLine &line : read_trn_key_lines(text))
		if (line.key == "polytrn_scale" && !line.values.empty()) scale = io::retail_atol(line.values.front().c_str());
	return true;
}

bool read_base_terrain(const FileSource &files, const std::string &terrain, int32_t &scale, TrnConfig &trn,
		std::vector<uint16_t> &atlas, uint64_t &cpt_stamp) {
	if (!read_base_trn(files, terrain + ".trn", trn, scale)) return false;
	std::vector<uint8_t> cpt_bytes;
	CptFile cpt;
	std::string error;
	if (trn.polydata.empty() || !files.read(trn.polydata, cpt_bytes) ||
	    !load_cpt(cpt_bytes.data(), cpt_bytes.size(), cpt, error))
		return false;
	atlas = std::move(cpt.depth_buffer);
	atlas.resize(size_t(1024) * 1024, 0);
	cpt_stamp = files.stamp(trn.polydata);
	return true;
}

mission::MisTerrain base_terrain_sampler(const TrnConfig &trn, int32_t scale, const uint16_t *heights) {
	mission::MisTerrain sampled;
	sampled.heights = heights;
	sampled.sectors = &trn.sector_grid[0][0];
	sampled.origin_x = trn.origin_x;
	sampled.origin_y = trn.origin_y;
	sampled.wrap_x = trn.wrap_x != 0;
	sampled.wrap_y = trn.wrap_y != 0;
	sampled.scale = scale;
	return sampled;
}

} // namespace opennova::editor
