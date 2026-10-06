#pragma once

#include <memory>

#include <editor/documents/project_check.h>

namespace opennova::editor {

// The sound bank type's project check (documents/project_check.h; the sound lane): each wave of the project
// the game's loader refuses, a warning on the file saying why in the loader's words (import/wave_source.h
// wave_retail_check [orig: Audio_LoadWavFileFromArchive @ 0x766480]): the game plays nothing for it and goes
// on, so it never blocks a build. A wave is read again only once its stamp moves (the file source's: the
// open documents standing in, the project's files by their size and last write).
std::unique_ptr<ProjectCheck> make_wave_check();

} // namespace opennova::editor
