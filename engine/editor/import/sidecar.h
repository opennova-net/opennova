#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <editor/import/importer.h>
#include <editor/model/diagnostic.h>

namespace opennova::editor {

// The `<file>.import` sidecar (ADR 0046 d6): committed beside its source, versioned
// UTF-8 JSON naming the importer, its version, the options, the outputs the last
// import produced, the content hash of the source they were made from and (S13 A8) every other
// file the import read with the content hash it had (`inputs`, each path the source folder's, a
// list written only when the import read any). It is what a modder commits with the source, so it
// holds nothing a checkout changes: the files' sizes and last-write times live in the
// machine-local import cache under `.opennova/` (import_run.h), beside the disposable outputs.
// The import pass rewrites it only when one of its fields changes.
inline constexpr int kImportSidecarSchemaVersion = 1;

struct ImportSidecar {
	std::string importer;
	int version = 0;
	ImportOptions options;
	uint64_t source_hash = 0;         // 64-bit FNV-1a of the source's bytes
	std::vector<ImportInput> inputs;  // the other files the import read, in the order it read them
	std::vector<std::string> outputs; // logical names, under the source's import directory

	bool operator==(const ImportSidecar &other) const {
		return importer == other.importer && version == other.version && options == other.options &&
		       source_hash == other.source_hash && inputs == other.inputs && outputs == other.outputs;
	}
	bool operator!=(const ImportSidecar &other) const { return !(*this == other); }
};

// A missing file reads as "never imported" (false with an empty error); an invalid
// one is an error.
bool load_import_sidecar(const std::string &path, ImportSidecar &out, Diagnostic &error);
bool save_import_sidecar(const std::string &path, const ImportSidecar &sidecar, Diagnostic &error);
// The record's identity: a hash of its canonical text (the bytes save writes), so a
// hand edit or a pulled change to any field reads as a different record while line
// endings or key order on disk do not.
uint64_t import_sidecar_fingerprint(const ImportSidecar &sidecar);

} // namespace opennova::editor
