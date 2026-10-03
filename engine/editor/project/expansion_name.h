#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <editor/model/diagnostic.h>
#include <editor/project/project_document.h>

namespace opennova::editor {

// The names a project's expansion (ADR 0046 S16) may take: the game's own limits, each refusal
// cited (docs/vfs/vfs-pff-mount-re.md § Expansions), and what a folder and an archive can hold.

// The longest name the game mounts: `/exp` copies its token into the 32-byte g_ExpansionName with
// strncpy, so a longer one is left unterminated, and the version.txt CRC written right after it then
// runs into every path made from the name, which no longer opens [orig: Game_ParseCommandLineAndInit
// @ 0x4a76ca; Expansion_LoadAssets @ 0x4a4885, the paths @ 0x4a48a6..0x4a49d4]; the join carries it
// in 32 bytes too [orig: UI_JoinSelectedSession @ 0x569afa, @ 0x569dc4]. Nothing shorter fails
// (D-VFS-8's search-path spill is read as one string, vfs-pff-mount-re.md § Expansions item 2).
inline constexpr size_t kExpansionNameMax = 31;

// What a name is for: the project's own expansion, whose files the build names after it (its music
// script M<n>.bin and its sound bank <n>L.lwf go into the archives, so they bind the archives'
// 16-byte names too), or the installed expansion it builds on (which the game only mounts).
enum class ExpansionNameUse { Own, BuildsOn };

// The first rule `name` breaks, in words ("" when it keeps them all):
// - 1..31 characters (kExpansionNameMax);
// - one `/exp` token: no space, tab or comma, which split the command line outside quotes, no `"`,
//   which a token never keeps, no `;`, which ends the line [orig: Terrain_TokenizeConfigLine
//   @ 0x53cb60 over GetCommandLineA, @ 0x4a73b2];
// - printable ASCII (the archives uppercase their names byte by byte [orig: PFF_SortEntries
//   @ 0x768280], and the game builds its paths in the ANSI code page);
// - a folder Windows can make: none of `\ / : * ? " < > |`, no trailing dot or space, not a device
//   name (CON, PRN, AUX, NUL, COM1..9, LPT1..9, alone or before a dot);
// - for the project's own (ExpansionNameUse::Own): M<n>.bin and <n>L.lwf fit the archives' names
//   (logical_name_fits_archive), so 11 characters [orig: Expansion_LoadAssets @ 0x4a491d, @ 0x4a4989].
std::string expansion_name_problem(std::string_view name, ExpansionNameUse use);

// expansion_name_problem as a finding: false with `error` (project.field.invalid) saying which rule
// `name` breaks.
bool check_expansion_name(const std::string &name, ExpansionNameUse use, Diagnostic &error);

// A project's expansion as its file may hold it, before any install is asked: none at all (a
// standalone project), or a name of the rule, and a `builds_on` only with a name and of the rule
// too (project.field.invalid); and only for the game the expansions were witnessed for, Joint
// Operations (project.expansion.unsupported: the other profiles' binaries are not researched).
bool check_project_expansion(const std::string &target_game, const ProjectExpansion &expansion,
                             Diagnostic &error);

// The expansion weighed against an install's expansions (vfs_list_expansions: the ones the game
// would mount), each name compared as the file system does, case-insensitively: the project's own
// name one the install has already (project.expansion.name_taken: building it would stand in for
// that one), the one it builds on one the install lacks (project.expansion.not_installed). Each at
// `severity`: a refusal where a project is made or its settings applied, a listed row where an open
// project's install is read again. Nothing for a standalone project.
void expansion_install_findings(const ProjectExpansion &expansion, const std::vector<std::string> &installed,
                                DiagnosticSeverity severity, std::vector<Diagnostic> &out);

} // namespace opennova::editor
