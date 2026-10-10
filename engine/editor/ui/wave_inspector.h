#pragma once

#include <memory>

#include <editor/ui/document_views.h>

namespace opennova::editor {

// A wave's Document tab (round S23 lane A; the wave type's row of ui/document_views): its picture drawn as large as
// the tab gives it (the loudest sample of each of its stretches, mirrored about the middle), what it is (its format,
// length, peak and RMS) and whether the game plays it and why not (the loader's own walk), a Play of it as the game
// loads it and a Stop, and its whole-wave edits: a trim (from and to, seconds) and a normalise (the loudest sample's
// level), each a wave_operation, one undo step, written in the form the game takes.
std::unique_ptr<DocumentView> make_wave_view();

} // namespace opennova::editor
