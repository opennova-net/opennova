#include <editor/import/converter.h>
#include <editor/session/finding_codes.h>

#include <filesystem>
#include <sstream>

#include <base/io/strutil.h>
#include <formats/bad/bad_build.h>
#include <formats/bad/bad_o3a_read.h>
#include <formats/threedi/threedi_o3d_read.h>
#include <runtime/anim/adm_clip_index.h>
#include <runtime/renderer/material_descriptor.h>
#include <runtime/renderer/material_texture.h>

namespace fs = std::filesystem;

namespace opennova::editor {
namespace {

// The scene reader's findings as the import's: an error makes no output.
void add_findings(const std::string &source_name, const std::vector<threedi::SceneFinding> &findings,
                  ImportProduct &out) {
	for (const threedi::SceneFinding &f : findings) {
		Diagnostic d = make_finding(f.error ? CoreFinding::ImportScene : CoreFinding::ImportSceneNote,
		                            f.error ? DiagnosticSeverity::Error : DiagnosticSeverity::Info,
		                            f.line > 0 ? "Line " + std::to_string(f.line) + ": " + f.message : f.message,
		                            source_name);
		d.line = static_cast<size_t>(f.line);
		out.diagnostics.push_back(std::move(d));
	}
}

bool failed(const ImportProduct &out) {
	for (const Diagnostic &d : out.diagnostics)
		if (d.severity == DiagnosticSeverity::Error) return true;
	return false;
}

// `.o3d`: the model through the engine's reader and mint (threedi_o3d_build).
bool run_o3d(const std::string &source_name, const std::vector<uint8_t> &bytes, ImportProduct &out) {
	std::istringstream text(std::string(bytes.begin(), bytes.end()));
	std::vector<threedi::SceneFinding> findings;
	ImportOutput model;
	model.name = fs::path(source_name).stem().generic_string() + ".3di";
	const bool built = threedi::threedi_o3d_build(text, renderer::material_descriptor_tangent_lookup,
	                                              renderer::material_texture_dds_only, model.bytes, findings);
	add_findings(source_name, findings, out);
	if (!built || failed(out)) {
		out.outputs.clear();
		return false;
	}
	out.outputs.push_back(std::move(model));
	return true;
}

// `.o3a`: the clip set through the engine's reader and set mint (bad_build_mint_set):
// the table its `adm` record names (else the source's stem) and every clip; one clip
// with no row is that clip alone.
bool run_o3a(const std::string &source_name, const std::vector<uint8_t> &bytes, ImportProduct &out) {
	std::istringstream text(std::string(bytes.begin(), bytes.end()));
	bad::BadBuildSet set;
	std::vector<threedi::SceneFinding> findings;
	std::vector<int> clip_lines;
	const bool read = bad::bad_o3a_read(text, anim::adm_slot_index, set, findings, &clip_lines);
	add_findings(source_name, findings, out);
	if (!read || failed(out)) return false;
	const bool lone = set.rows.empty() && set.clips.size() == 1;
	const std::string name = lone ? set.clips[0].name + ".bad"
	                         : set.adm_name.empty() ? fs::path(source_name).stem().generic_string() + ".adm"
	                                                : set.adm_name;
	std::vector<bad::BadMintedFile> files;
	std::string error;
	int failed_clip = -1;
	if (!bad::bad_build_mint_set(set, name, files, &error, &failed_clip)) {
		Diagnostic d = make_finding(CoreFinding::ImportScene, DiagnosticSeverity::Error,
		                            failed_clip >= 0 ? "Line " + std::to_string(clip_lines[failed_clip]) + ": " + error : error,
		                            source_name);
		if (failed_clip >= 0) d.line = static_cast<size_t>(clip_lines[failed_clip]);
		out.diagnostics.push_back(std::move(d));
		return false;
	}
	for (bad::BadMintedFile &file : files) out.outputs.push_back({file.name, std::move(file.bytes)});
	return true;
}

} // namespace

const std::vector<Converter> &converters() {
	static const std::vector<Converter> table = {
		{"o3d", {".o3d"}, run_o3d},
		{"o3a", {".o3a"}, run_o3a},
	};
	return table;
}

const Converter *converter_for(const std::string &source_name) {
	const std::string extension = strutil::to_lower(fs::path(source_name).extension().generic_string());
	if (extension.empty()) return nullptr;
	for (const Converter &converter : converters())
		for (const std::string &candidate : converter.extensions)
			if (candidate == extension) return &converter;
	return nullptr;
}

} // namespace opennova::editor
