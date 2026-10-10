#include <editor/import/converter.h>

#include <sstream>

#include <filesystem>
#include <system_error>

#include <base/io/strutil.h>
#include <editor/model/diagnostic.h>
#include <editor/project/project_files.h>
#include <formats/bad/bad_build.h>
#include <formats/bad/bad_o3a_read.h>
#include <formats/threedi/threedi_o3d_lower.h>
#include <formats/threedi_gp/threedi_gp.h>
#include <formats/threedi_gp/threedi_gp_migrate.h>
#include <runtime/anim/adm_clip_index.h>
#include <runtime/renderer/model_target.h>


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

// `.o3d`: the model through the engine's reader, its lowering to the retail target and
// the mint (threedi_o3d_build over renderer::retail_model_target).
bool run_o3d(const std::string &source_name, const std::string &, const std::vector<uint8_t> &bytes,
             ImportProduct &out) {
	std::istringstream text(std::string(bytes.begin(), bytes.end()));
	std::vector<threedi::SceneFinding> findings;
	ImportOutput model;
	model.name = utf8_of(path_of(source_name).stem()) + ".3di";
	const bool built = threedi::threedi_o3d_build(text, renderer::retail_model_target(), model.bytes, findings);
	add_findings(source_name, findings, out);
	if (!built || failed(out)) {
		out.outputs.clear();
		return false;
	}
	out.outputs.push_back(std::move(model));
	return true;
}

// `.o3a`: the clip set through the engine's reader and its mint for the retail target
// (bad_build_mint_set over bad_retail_limits and the runtime's anim slots): the table its
// `adm` record names (else the source's stem) and every clip; one clip with no row is
// that clip alone. Every problem the target finds names its line.
bool run_o3a(const std::string &source_name, const std::string &, const std::vector<uint8_t> &bytes,
             ImportProduct &out) {
	std::istringstream text(std::string(bytes.begin(), bytes.end()));
	bad::BadBuildSet set;
	std::vector<threedi::SceneFinding> findings;
	bad::BadO3aLines lines;
	const bool read = bad::bad_o3a_read(text, set, findings, &lines);
	add_findings(source_name, findings, out);
	if (!read || failed(out)) return false;
	const bool lone = set.rows.empty() && set.clips.size() == 1;
	const std::string name = lone ? set.clips[0].name + ".bad"
	                         : set.adm_name.empty() ? utf8_of(path_of(source_name).stem()) + ".adm"
	                                                : set.adm_name;
	std::vector<bad::BadMintedFile> files;
	std::vector<bad::BadBuildProblem> problems;
	if (!bad::bad_build_mint_set(set, name, bad::bad_retail_limits(), anim::adm_slot_index, files, problems)) {
		std::vector<threedi::SceneFinding> refused;
		bad::bad_o3a_findings(lines, problems, refused);
		add_findings(source_name, refused, out);
		return false;
	}
	for (bad::BadMintedFile &file : files) out.outputs.push_back({file.name, std::move(file.bytes)});
	return true;
}

bool claims_gp(const std::vector<uint8_t> &bytes) {
	return threedi_gp::detect(bytes.data(), bytes.size()) != threedi_gp::Kind::None;
}

// A GP `.3di`: the model through the engine's GP reader and its migration to 3DI3, under the
// source's own name; each note the migration makes is a note of the import, its count in its
// words. A bump material's normal map is the `.mdt` beside the source when there is one, as
// BHD's loader tries (threedi_gp_migrate.h).
bool run_gp(const std::string &source_name, const std::string &source_path, const std::vector<uint8_t> &bytes,
            ImportProduct &out) {
	threedi_gp::File file;
	std::string error;
	ImportOutput model;
	model.name = source_name;
	threedi_gp::MigrateOptions options;
	if (!source_path.empty()) {
		std::filesystem::path folder = path_of(source_path).parent_path();
		if (folder.empty()) folder = ".";
		options.texture_exists = [folder](const std::string &name) {
			std::error_code ec;
			for (const auto &entry : std::filesystem::directory_iterator(folder, ec))
				if (strutil::iequals(utf8_of(entry.path().filename()), name)) return true;
			return false;
		};
	}
	std::vector<threedi_gp::MigrateNote> notes;
	if (!threedi_gp::parse(bytes.data(), bytes.size(), file, error) ||
	    !threedi_gp::migrate(file, model.bytes, notes, error, options)) {
		out.diagnostics.push_back(make_finding(CoreFinding::ImportMigrate, DiagnosticSeverity::Error,
		                                       "The Black Hawk Down model could not be migrated to 3DI3: " + error + ".",
		                                       source_name));
		return false;
	}
	for (const threedi_gp::MigrateNote &n : notes)
		out.diagnostics.push_back(make_finding(
		        CoreFinding::ImportMigrateNote, DiagnosticSeverity::Info,
		        "Migrated to 3DI3: " + n.text + (n.count > 1 ? " (" + std::to_string(n.count) + " times)" : "") + ".",
		        source_name));
	out.outputs.push_back(std::move(model));
	return true;
}

} // namespace

const std::vector<Converter> &converters() {
	static const std::vector<Converter> table = {
		{"o3d", {".o3d"}, nullptr, run_o3d},
		{"o3a", {".o3a"}, nullptr, run_o3a},
		{"gp_model", {".3di"}, claims_gp, run_gp},
	};
	return table;
}

const Converter *converter_for(const std::string &source_name, const std::vector<uint8_t> *bytes) {
	const std::string extension = strutil::to_lower(utf8_of(path_of(source_name).extension()));
	if (extension.empty()) return nullptr;
	for (const Converter &converter : converters())
		for (const std::string &candidate : converter.extensions)
			if (candidate == extension && (!converter.claims || (bytes != nullptr && converter.claims(*bytes))))
				return &converter;
	return nullptr;
}

} // namespace opennova::editor
