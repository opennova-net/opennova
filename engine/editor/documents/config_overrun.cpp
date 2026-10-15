#include "config_overrun.h"

#include <string>

#include <editor/assets/asset_kinds.h>
#include <editor/documents/text_types.h>
#include <editor/model/core_finding.h>
#include <editor/model/text_document.h>
#include <editor/project/project_files.h>
#include <formats/configfile/config_file.h>

namespace opennova::editor {

namespace {

configfile::DataStringsPool pool_of(const std::string &text) {
	return configfile::data_strings_pool(reinterpret_cast<const uint8_t *>(text.data()), text.size());
}

std::string count_of(size_t n, const char *one, const char *many) {
	return std::to_string(n) + " " + (n == 1 ? one : many);
}

} // namespace

std::string config_overrun_words(const std::string &file, const configfile::DataStringsPool &pool) {
	std::string words = file + " holds " + count_of(pool.values, "value", "values") +
	                    (pool.string_bytes == 0 ? std::string(" and no text value")
	                                            : ", and its text values take " +
	                                                      count_of(pool.string_bytes, "byte", "bytes") +
	                                                      " (each its length and one)");
	words += ": the game's ConfigFile reader keeps those in a buffer of " + std::to_string(pool.pool_bytes) +
	         " bytes (rounded up to its allocator's 64) and then clears that buffer one byte per value, so it "
	         "writes " +
	         count_of(pool.overrun(), "byte", "bytes") +
	         " of zeros past the buffer into the game's memory, and the game crashes later, as a mission starts "
	         "(ConfigFile_ParseText @ 0x7609e8). Value " +
	         std::to_string(pool.pool_bytes + 1) + ", here, is the first past it.";
	return words;
}

std::vector<Diagnostic> config_overrun_findings(const DocumentBase &document) {
	std::vector<Diagnostic> findings;
	if (file_kind_facts(document.kind()).line_reader != LineReader::ConfigFile) return findings;
	const TextDocument *text = text_of(document);
	// A file stored in another form (a credits file's CBIN, its encoding) is not read by the text reader.
	if (!text || document.blocked() || text->encoding()) return findings;
	const std::string &written = text->text();
	const configfile::DataStringsPool pool = pool_of(written);
	if (pool.binary || pool.overrun() == 0) return findings;
	const std::string file = basename_of(document.path());
	const std::vector<configfile::ConfigSection> sections =
			configfile::parse_config_text(reinterpret_cast<const uint8_t *>(written.data()), written.size());
	// The value whose byte of the clear is the first past the pool, in the reader's order.
	const size_t offset = configfile::data_strings_overrun_offset(sections, pool);
	Diagnostic d = text_finding(finding_code(CoreFinding::DocumentConfigOverrun), DiagnosticSeverity::Error,
	                            config_overrun_words(file, pool), *text, offset);
	findings.push_back(std::move(d));
	return findings;
}

} // namespace opennova::editor
