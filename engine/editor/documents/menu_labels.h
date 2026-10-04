#pragma once

#include <string>

#include <editor/documents/name_source.h>
#include <editor/model/document.h>

namespace opennova::editor {

// A menu's parts in a modder's words (ADR 0046, the UX round's plain-words lane; the menu type's
// DocumentType::record_label), where they read "Action 1", "Appearance 2", "Sound 1" before: an
// action as what it does (docs/mnu/menu-re.md "Authored Actions" [orig: CUIElement_ParseXMLDefinition
// @ 0x648ee2; CUIWidget_HandleScriptedAction @ 0x6497f0]: "Go to OPTIONS in options.mnu", "Show
// SERVER_INFO", "Go back", "Does nothing" for a type no code acts on), an appearance as its state and
// its look ("Mouse over: image btn1o.tga"), a sound as when it plays and what ("Mouse enters:
// MOUSE_OVER"), a hotkey by its key, a data source by its file, a list's item and a table's cell by
// their text, a header by its words, a substitution by what it shows. "" for a screen and a window,
// whose names say them, and for a record its fields cannot word.
std::string menu_record_label(const Document &document, const NodeAddress &address, const NameSource *names);

} // namespace opennova::editor
