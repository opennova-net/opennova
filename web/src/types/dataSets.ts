// The hosted data sets' admin routes (/api/admin/data-sets*;
// apps/novaworld_web/data_sets, ADR 0064), in the server's snake_case. A data
// set is one product an admin created: a base game (JO, DFX2) or one
// expansion of a base (Escalation, jox01). Its trees are the immutable
// versions of its files in the object store, each one an admin uploaded.
// Times are the service's UTC 'YYYY-MM-DD HH:MM:SS' (utils/format.ts), null
// until set.

export type DataSetKind = 'base' | 'expansion';

// uploading (the browser sends its files) -> finalizing (the worker checks
// and lists them) -> ready; failed names its error; pruned once its objects
// are gone.
export type TreeState = 'uploading' | 'finalizing' | 'ready' | 'failed' | 'pruned';

export interface Tree {
  id: number;
  state: TreeState | (string & {});
  // The tree's version.txt.
  version: string;
  bytes: number;
  // How many files and how many multiplayer missions it holds.
  files: number;
  missions: number;
  // How many live servers run on it.
  pinned_by: number;
  // Why it failed; '' otherwise.
  error: string;
  created_by: string;
  created_at: string;
  ready_at: string | null;
}

export interface CurrentTree {
  id: number;
  version: string;
  ready_at: string | null;
}

export interface DataSet {
  id: number;
  display_name: string;
  kind: DataSetKind;
  // An expansion's base; null for a base.
  parent_id: number | null;
  // An expansion's /exp code ('jox01'); '' for a base.
  expansion_code: string;
  // A base's title ('jo', 'dfx2'); '' for an expansion (it speaks its parent's).
  title: string;
  // A base's games slug, the lobby it lists in; '' for an expansion.
  game: string;
  // What new servers start on; null until a tree is ready.
  current_tree: CurrentTree | null;
  // Live servers running it, and how many of them run on an older tree.
  servers_live: number;
  servers_outdated: number;
  trees: Tree[];
}

export interface GameOption {
  slug: string;
  display_name: string;
}

// GET /api/admin/data-sets.
export interface DataSetsResponse {
  // Where the trees live.
  store: { kind: 's3' | 'local' | (string & {}) };
  // The titles a base may speak.
  titles: string[];
  // The lobbies a base may list in.
  games: GameOption[];
  data_sets: DataSet[];
}

// POST /api/admin/data-sets's body.
export interface NewDataSet {
  display_name: string;
  kind: DataSetKind;
  parent_id?: number;
  expansion_code?: string;
  title?: string;
  game?: string;
}

// PATCH /api/admin/data-sets/<id>'s body: any of these.
export interface DataSetPatch {
  display_name?: string;
  game?: string;
}

// One file of a tree upload, as POST .../trees names it.
export interface TreeUploadFile {
  name: string;
  size: number;
}

// One file's multipart upload, as POST .../trees opened it.
export interface TreeUpload {
  // The canonical name.
  name: string;
  key: string;
  upload_id: string;
  // The part size the browser must use (32 MiB).
  part_size: number;
}

export interface NewTreeReply {
  tree: { id: number };
  uploads: TreeUpload[];
}

// One uploaded part, as the complete route takes it.
export interface UploadedPart {
  part_number: number;
  // The ETag the store answered the part with.
  etag: string;
}
