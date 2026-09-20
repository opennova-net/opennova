#pragma once

#include <string>

#include <editor/model/diagnostic.h>

namespace opennova::editor {

// Machine-local, never-committed settings of one project (ADR 0046 d6):
// `.opennova/local.json`. Paths here are absolute on this machine; nothing the build
// output depends on lives here.
inline constexpr int kLocalSettingsSchemaVersion = 1;

struct LocalSettings {
	std::string runtime_executable; // the opennova.exe Play launches ("" = beside the editor)
	std::string retail_root;        // an installed game to import from / depend on ("" = none)
};

// A missing file reads as defaults; a present but invalid file is an error.
bool load_local_settings(const std::string &path, LocalSettings &out, Diagnostic &error);
bool save_local_settings(const std::string &path, const LocalSettings &settings, Diagnostic &error);

} // namespace opennova::editor
