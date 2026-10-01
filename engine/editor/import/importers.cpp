// The importer table (ADR 0046 d10, S8). The image importer: a PNG becomes the 256-color PCX
// the game's texture and menu loaders read, or (format tga, S13 A8) the 32-bit TGA they read
// too, its alpha kept (formats/tga's writer: the one shape the game's TGA readers all take).
#include <editor/import/importer.h>

#include <filesystem>

#include <base/io/strutil.h>
#include <editor/import/png_decode.h>
#include <editor/import/quantize.h>
#include <editor/model/diagnostic.h>
#include <formats/pcx/pcx_io.h>
#include <formats/tga/tga.h>

namespace opennova::editor {

namespace {

bool run_image(ImportContext &context, ImportProduct &out) {
	const std::string &source_name = context.source_name();
	const auto format = context.options().find("format");
	const std::string wanted = format == context.options().end() ? std::string("pcx") : strutil::to_lower(format->second);
	if (wanted != "pcx" && wanted != "tga") {
		out.diagnostics.push_back(make_finding(CoreFinding::ImportOption, DiagnosticSeverity::Error,
		                                       "The image importer writes pcx or tga; '" + wanted +
		                                               "' is neither (set format to pcx or tga).",
		                                       source_name, "format"));
		return false;
	}
	RgbaImage image;
	std::string error;
	if (!decode_png(context.source(), image, error)) {
		out.diagnostics.push_back(make_finding(CoreFinding::ImportDecode, DiagnosticSeverity::Error, error, source_name));
		return false;
	}
	ImportOutput output;
	output.name = std::filesystem::path(source_name).stem().generic_string() + "." + wanted;
	if (wanted == "tga") {
		if (!tga::tga_write_rgba32(image.pixels.data(), static_cast<uint32_t>(image.width),
		                           static_cast<uint32_t>(image.height), output.bytes, error)) {
			out.diagnostics.push_back(make_finding(CoreFinding::ImportEncode, DiagnosticSeverity::Error, error, source_name));
			return false;
		}
		out.outputs.push_back(std::move(output));
		return true;
	}
	bool translucent = false;
	for (size_t i = 3; i < image.pixels.size(); i += 4)
		if (image.pixels[i] != 255) { translucent = true; break; }
	if (translucent)
		out.diagnostics.push_back(make_finding(CoreFinding::ImportAlphaDropped, DiagnosticSeverity::Warning,
		                                       "PCX carries no alpha: the transparency of " + source_name +
		                                               " is dropped (format tga keeps it).",
		                                       source_name));
	const IndexedImage8 indexed = quantize_to_256(image);
	if (!encode_pcx_indexed(indexed, output.bytes, error)) {
		out.diagnostics.push_back(make_finding(CoreFinding::ImportEncode, DiagnosticSeverity::Error, error, source_name));
		return false;
	}
	out.outputs.push_back(std::move(output));
	return true;
}

} // namespace

const std::vector<Importer> &importers() {
	static const std::vector<Importer> table = [] {
		std::vector<Importer> rows;
		Importer image;
		image.id = "image";
		image.version = 1;
		image.extensions = {".png"};
		image.default_options = {{"format", "pcx"}};
		image.run = run_image;
		rows.push_back(std::move(image));
		return rows;
	}();
	return table;
}

const Importer *importer_for(const std::string &source_name) {
	return importer_for(source_name, importers());
}

const Importer *importer_for(const std::string &source_name, const std::vector<Importer> &table) {
	const std::string extension = strutil::to_lower(std::filesystem::path(source_name).extension().generic_string());
	if (extension.empty()) return nullptr;
	for (const Importer &importer : table)
		for (const std::string &candidate : importer.extensions)
			if (candidate == extension) return &importer;
	return nullptr;
}

std::string renamed_import_output(const std::string &output, const std::string &old_source,
                                  const std::string &new_source) {
	const std::filesystem::path path(output);
	if (strutil::to_lower(path.stem().generic_string()) !=
	    strutil::to_lower(std::filesystem::path(old_source).stem().generic_string()))
		return output;
	return std::filesystem::path(new_source).stem().generic_string() + path.extension().generic_string();
}

} // namespace opennova::editor
