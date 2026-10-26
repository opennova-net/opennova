#pragma once

#include <string>
#include <vector>

#include <editor/graph/reference_queries.h>
#include <editor/model/diagnostic.h>
#include <editor/model/field_use.h>
#include <editor/model/value.h>
#include <editor/ui/reference_picker.h>
#include <editor/ui/workspace.h>

namespace opennova::editor {

struct SessionView;

// A name drawn where the editor shows it, as every reference is (ADR 0046 DI-17 and DI-18, the deep integration the
// maintainer asked for: every name a field holds is a jump): a record's field (the Inspector's, by its FieldUse and value), a name a readout
// shows (a viewport's board, log or listen line: a ReferenceSubject, the kind, the name, its scope and its loader's
// argument, as a Problems row's subject names one), and a name a text writes at a span (a particle file's, a HUD
// layout's: a TextPlace). Each resolves through the graph's one lookup (reference_queries' reference_targets: a
// readout's name as a field naming it there), so a jump goes where the field's Go to would, through
// window_requests::go_to (open_document: Back and Forward record it).

// Where a Go to leads, in its tooltip: the file opened at the record, or its page (DI-17); one of several.
std::string go_to_words(const std::vector<ReferenceTarget> &targets);

// Where a reference's jumps lead (DI-18): what the game's lookup reaches (reference_targets); a name nothing
// resolves, where it belongs (missing_target: DI-17's Go to that always lands). None without a graph.
std::vector<ReferenceTarget> jump_targets(const SessionView &view, const FieldUse &field, const Value &value);
std::vector<ReferenceTarget> jump_targets(const SessionView &view, const ReferenceSubject &name);

// A reference's Go to, to the places the game's lookup reaches (made only while the tool is hovered, pressed or its
// menu open): one gone to, or several offered in a menu (a font through a style variable: the variable where the
// game reads it, or the file its value names). `pressed` is the tool's press (the item just drawn); its tooltip is
// `tip`, then where it leads and its keys.
void go_to_tool(Workspace &workspace, const FieldUse &field, const Value &value, bool pressed, const std::string &tip);
void go_to_tool(Workspace &workspace, const ReferenceSubject &name, bool pressed, const std::string &tip);

// A reference's jumps over the item just drawn (its value's control, drawn after SetNextItemAllowOverlap): while it
// is under the pointer or has the keyboard, what F12 (Go to definition) and Shift+F12 (Find usages) act on, the uses
// those of what it names; with Ctrl held over it a hand, and a click there its Go to, taken from the control under it
// (an invisible button over it, so the click neither types nor opens a list); and its right-click menu: Go to
// definition, Find usages, Show in Files.
void reference_jumps(Workspace &workspace, const FieldUse &field, const Value &value);
void reference_jumps(Workspace &workspace, const ReferenceSubject &name);

// A name a readout shows (an impact row's effect, a log line's sound set, Listen's bank), drawn as a link: its text
// (in the missing colour where the game's lookup finds nothing, "none" muted where it names nothing), a click its Go
// to, and its jumps (reference_jumps). `id` keeps two links of one name apart; `tip` comes before where it leads.
// True when it was clicked.
bool name_link(Workspace &workspace, const ReferenceSubject &name, const char *id, const std::string &tip = std::string());

// A name a text writes at a span, edited in place (the particle panel's names, the HUD layout's font): a box holding
// it, completing as it is typed (ReferencePicker::draw_completions over the place), with its state's dot (a click
// its Go to, or a missing name's fixes: ReferencePicker::draw_fixes), its Pick, a Files row dropped on it, and its
// jumps over it. The box takes the width left less the tools'. True when a name was given this frame (Enter on a
// name typed, a completion taken, a pick, a drop), `given` holding it: the caller replaces the span with it (a
// particle file's value checked as its reader takes it, particle_name_edit).
bool text_reference_field(Workspace &workspace, ReferencePicker &picker, const TextPlace &place, std::string &given);

} // namespace opennova::editor
