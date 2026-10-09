#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace opennova::filelog {

// The game install's own file log, `_filelog.txt` in its working directory, which `/FRISK` turns on
// [orig: Game_ParseCommandLineAndInit @ 0x4a768c -> File_SetLoggingEnabled @ 0x75a470]: a line for
// every file the game's file layer OPENED, appended as it opens it
// [orig: File_LogFileAccess @ 0x75a480]: "PFF LOADED FILE: <name>" for an entry an archive served
// [orig: PFF_OpenFile @ 0x7688da], "LOADED FILE: <path>" for a file opened from disk, an archive itself
// among them [orig: File_OpenRead @ 0x75a639 (the archives through PFF_Open @ 0x768330, the saves
// through PlayerProfile_LoadAllFromDisk @ 0x54f5a0); FileSystem_OpenFile @ 0x75b2d6;
// FileSystem_GetFileSize @ 0x75b436; FileSystem_ReadFileEx @ 0x75b91b], so one file may be logged more
// than once. Only an open that succeeded is logged (each call follows a handle that is not -1, an entry
// PFF_FindEntry found): a file the game looked for and did not find never is, so the log cannot say
// what the game lacked. A file the game opens outside that layer is not logged either: its
// configuration, read through the C runtime's fopen
// [orig: File_ParseASCIIFileWithCallback @ 0x53d980, from Game_LoadConfig @ 0x551499].
//
// The game opens the log exclusively for each line (`_lopen(OF_WRITE | OF_SHARE_EXCLUSIVE)`, then
// `_llseek` to its end) and, when that open fails, makes the file anew with `_lcreat`, which
// truncates it; its first line deletes the log a run before left
// [orig: File_LogFileAccess @ 0x75a4c2 DeleteFileA, @ 0x75a4d2 _lopen(0x11), @ 0x75a4e8 _lcreat].
// A reader holding the file open while the game appends (a tail) therefore cuts the log to its last
// line: read it once the game has exited, never while it runs.
inline constexpr const char *kInstallFileLogName = "_filelog.txt";

// What a file log says the game loaded, each name once (the first spelling, compared without case
// as the game's file calls compare them), in the order the game first opened it.
struct FileAccessLog {
	size_t lines = 0;                       // the log's lines
	std::vector<std::string> archives;      // the archives the game opened from disk (a .pff)
	std::vector<std::string> from_archives; // the files the archives served
	std::vector<std::string> from_disk;     // the files opened from disk but the archives themselves
	bool operator==(const FileAccessLog &o) const {
		return lines == o.lines && archives == o.archives && from_archives == o.from_archives &&
		       from_disk == o.from_disk;
	}
};

// One line of a file log added to `log` (its line end already cut, a '\r' too); a line neither form
// starts is counted and nothing else.
void add_file_access_line(FileAccessLog &log, const std::string &line);
// A whole file log's text read into a FileAccessLog (lines ended by "\n", the game's, or "\r\n").
FileAccessLog parse_file_access_log(const std::string &text);

} // namespace opennova::filelog
