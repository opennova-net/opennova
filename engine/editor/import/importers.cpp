// The importer table (ADR 0046 d10, S8). The image importer (import/texture_import, S18): a PNG made into
// the texture file its uses read, by its record's options; a TGA or a PCX too, where a record makes it a
// source (Replace, Edit externally). The wave importer (import/wave_source, the sound lane): a wave its
// record makes a source written in the form the game plays.
#include <editor/import/importer.h>

#include <filesystem>

#include <base/io/strutil.h>
#include <editor/import/texture_import.h>
#include <editor/import/wave_source.h>
#include <editor/project/project_files.h>

namespace opennova::editor {

const std::vector<Importer> &importers() {
	static const std::vector<Importer> table = [] {
		std::vector<Importer> rows;
		Importer image;
		image.id = "image";
		image.version = kImageImporterVersion;
		image.extensions = {".png"};
		image.record_extensions = {".tga", ".pcx"};
		image.options = image_import_option_rows();
		image.run = run_image_import;
		rows.push_back(std::move(image));
		// The sound lane: a wave a record makes a source of, written in the form the game plays.
		Importer wave;
		wave.id = "wave";
		wave.version = kWaveImporterVersion;
		wave.record_extensions = {".wav"};
		wave.default_options = {{"channels", "mono"}, {"bits", "keep"}, {"rate", "keep"}};
		wave.options = wave_import_option_rows();
		wave.run = run_wave_import;
		rows.push_back(std::move(wave));
		return rows;
	}();
	return table;
}

const Importer *importer_for(const std::string &source_name) {
	return importer_for(source_name, importers());
}

namespace {

const Importer *importer_by_extension(const std::string &source_name, const std::vector<Importer> &table, bool records) {
	const std::string extension = strutil::to_lower(utf8_of(path_of(source_name).extension()));
	if (extension.empty()) return nullptr;
	for (const Importer &importer : table) {
		for (const std::string &candidate : importer.extensions)
			if (candidate == extension) return &importer;
		if (records)
			for (const std::string &candidate : importer.record_extensions)
				if (candidate == extension) return &importer;
	}
	return nullptr;
}

} // namespace

const Importer *importer_for(const std::string &source_name, const std::vector<Importer> &table) {
	return importer_by_extension(source_name, table, true);
}

const Importer *authored_importer_for(const std::string &source_name) {
	return importer_by_extension(source_name, importers(), false);
}

const ImportOptionRow *import_option_row(const std::vector<ImportOptionRow> &rows, const std::string &key) {
	for (const ImportOptionRow &row : rows)
		if (row.key == key) return &row;
	return nullptr;
}

bool import_option_accepts(const ImportOptionRow &row, const std::string &value) {
	for (const ImportOptionValue &each : row.values)
		if (each.token == value) return true;
	return row.accepts && row.accepts(value);
}

std::string import_option_takes(const ImportOptionRow &row) {
	std::vector<std::string> all;
	for (const ImportOptionValue &each : row.values) all.push_back(each.token);
	for (const std::string &form : row.forms) all.push_back(form);
	std::string out;
	for (size_t i = 0; i < all.size(); ++i) out += (i == 0 ? "" : i + 1 == all.size() ? " or " : ", ") + all[i];
	return out;
}

std::string renamed_import_output(const std::string &output, const std::string &old_source,
                                  const std::string &new_source) {
	const std::filesystem::path path = path_of(output);
	const std::string stem = utf8_of(path.stem());
	const std::string old_stem = utf8_of(path_of(old_source).stem());
	const std::string new_stem = utf8_of(path_of(new_source).stem());
	// The source's stem itself, or followed by an underscore and the rest (`<stem>_01`).
	const bool named = strutil::to_lower(stem) == strutil::to_lower(old_stem) ||
	                   (stem.size() > old_stem.size() + 1 && stem[old_stem.size()] == '_' &&
	                    strutil::to_lower(stem.substr(0, old_stem.size())) == strutil::to_lower(old_stem));
	if (!named) return output;
	return new_stem + stem.substr(old_stem.size()) + utf8_of(path.extension());
}

} // namespace opennova::editor
