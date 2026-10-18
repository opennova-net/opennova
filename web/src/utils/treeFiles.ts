// The files a data set's tree holds, by kind (apps/novaworld_common/data_sets/
// data_set_files.h, ADR 0064), mirrored for the admin page's folder picker:
// what it keeps out of a picked folder, what it says is missing, and how
// many files it leaves out. The upload route refuses any other name and the
// worker's finalize checks the required ones, so the lists here must match
// the service's.

import type { DataSetKind } from '../types/dataSets';

export interface TreeFileRule {
  // The canonical spelling, as the tree stores it.
  name: string;
  required: boolean;
}

// A base game's tree: the three archives Vfs::mount_game needs, then the
// loose files the server reads when the install has them.
const BASE_RULES: readonly TreeFileRule[] = [
  { name: 'resource.pff', required: true },
  { name: 'localres.pff', required: true },
  { name: 'language.pff', required: true },
  { name: 'version.txt', required: false },
  { name: 'score.ini', required: false },
  { name: '_NSTMOUT.TXT', required: false },
  { name: 'gt.ssc', required: false },
];

// The kind's files, required ones first. `code` names an expansion's pair
// ('jox01': jox01.pff and jox01L.pff).
export function treeFileRules(kind: DataSetKind, code: string): TreeFileRule[] {
  if (kind === 'base') return [...BASE_RULES];
  return [
    { name: `${code}.pff`, required: true },
    { name: `${code}L.pff`, required: true },
    { name: 'version.txt', required: true },
    { name: 'gt.ssc', required: false },
  ];
}

// The canonical spelling of `name` among the kind's files (compared without
// case: a retail install spells _NSTMOUT.TXT and jox01L.pff its own way), or
// null when the kind takes no file of that name.
export function canonicalTreeFile(kind: DataSetKind, code: string, name: string): string | null {
  const wanted = name.toLowerCase();
  return treeFileRules(kind, code).find((rule) => rule.name.toLowerCase() === wanted)?.name ?? null;
}

// A file the picker keeps, under its canonical name.
export interface KeptTreeFile {
  name: string;
  file: File;
}

export interface PickedTreeFiles {
  // In the kind's order (required first).
  kept: KeptTreeFile[];
  // The required names the pick lacks.
  missing: string[];
  // How many picked files were left out: other names, and files in subfolders.
  ignored: number;
}

// A folder picked with webkitdirectory gives each file a path relative to
// the folder's parent ('JointOps/resource.pff'); a plain multiple pick gives
// none. Only the picked folder's own files count: a base takes the files at
// the install's root, and an expansion the files of its own folder (the admin
// picks expansion/jox01), never anything deeper.
function atFolderRoot(file: File): boolean {
  const path = (file as File & { webkitRelativePath?: string }).webkitRelativePath ?? '';
  if (path === '') return true;
  const parts = path.split('/');
  return parts.length === 2;
}

// The files of a pick that make a tree of the kind, the required names it
// lacks and how many it left out. A name picked twice (a plain pick could)
// keeps the first.
export function pickTreeFiles(kind: DataSetKind, code: string, files: readonly File[]): PickedTreeFiles {
  const byName = new Map<string, File>();
  for (const file of files) {
    if (!atFolderRoot(file)) continue;
    const name = canonicalTreeFile(kind, code, file.name);
    if (name === null || byName.has(name)) continue;
    byName.set(name, file);
  }
  const rules = treeFileRules(kind, code);
  const kept: KeptTreeFile[] = [];
  const missing: string[] = [];
  for (const rule of rules) {
    const file = byName.get(rule.name);
    if (file) kept.push({ name: rule.name, file });
    else if (rule.required) missing.push(rule.name);
  }
  return { kept, missing, ignored: files.length - kept.length };
}
