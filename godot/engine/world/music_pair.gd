class_name MusicPair
extends RefCounted

## One resolved interactive-music pair: the loose .sbf bank path plus the VFS
## .bin script basename, always from the SAME stem — the script's play ops
## index that bank's entries, so halves are never mixed (ADR 0017 typed record
## behind NovaMusicService.resolve_music_pair). An empty half means
## "unresolved"; the context open then bails to silence, matching the
## witnessed .sbf CreateFileA gate [orig: AudioVM_OpenContextFile @ 0x672160].

var bank := ""         # loose .sbf path ("" = unresolved)
var script_name := ""  # VFS .bin basename ("" = unresolved; "script" is Object's own property)
