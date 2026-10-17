// The hosted data sets' admin routes (/api/admin/data-sets*, /api/admin/releases*;
// apps/novaworld_web/data_sets, ADR 0064), in the server's snake_case. A data
// set is one product an admin created: a base game (JO, DFX2) or one
// expansion of a base (Escalation, jox01). Its trees are the immutable
// versions of its files in the object store: a baseline the admin uploaded, or
// a published release applied to the baseline. Times are the service's UTC
// 'YYYY-MM-DD HH:MM:SS' (utils/format.ts), null until set.

export type DataSetKind = 'base' | 'expansion';

// uploading (the browser sends its files) -> finalizing (the worker checks
// and lists them) -> ready; a release's tree is building while the worker
// applies it; failed names its error; pruned once its objects are gone.
export type TreeState = 'uploading' | 'finalizing' | 'building' | 'ready' | 'failed' | 'pruned';

export type TreeSource = 'baseline' | 'release';

export interface Tree {
  id: number;
  state: TreeState | (string & {});
  source: TreeSource | (string & {});
  // The release it was built from; null for a baseline.
  release_id: number | null;
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

export interface PublishedRelease {
  id: number;
  name: string;
  sha256: string;
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
  // The updater's login and command file, both '' when the update service
  // does not serve it.
  updater_login: string;
  command_file: string;
  // SERVER.LST names NovaLogic's ftp.novaworld.com rather than this service's
  // update host.
  retail_updater: boolean;
  // What new servers start on; null until a tree is ready.
  current_tree: CurrentTree | null;
  // What a release applies to.
  baseline_tree_id: number | null;
  published_release: PublishedRelease | null;
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
  updater_login?: string;
  command_file?: string;
  retail_updater?: boolean;
}

// PATCH /api/admin/data-sets/<id>'s body: any of these.
export interface DataSetPatch {
  display_name?: string;
  game?: string;
  updater_login?: string;
  command_file?: string;
  retail_updater?: boolean;
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

// uploading -> validating -> draft | invalid; draft -> published -> retired.
export type ReleaseStatus = 'uploading' | 'validating' | 'invalid' | 'draft' | 'published' | 'retired';

// One section of a release: one product's update, mapped to a data set by its
// command file.
export interface ReleaseSection {
  data_set_id: number;
  display_name: string;
  // The PACKAGEBREAK name; '' for a base section.
  expansion: string;
  command_file: string;
  // The version the section brings.
  version: string;
  // The tree its publish built; null before.
  tree: { id: number; state: TreeState | (string & {}) } | null;
}

export interface Release {
  id: number;
  status: ReleaseStatus | (string & {});
  name: string;
  // Hex; '' until validated.
  sha256: string;
  bytes: number;
  // The package's product name.
  package_product: string;
  // Why it is invalid; '' otherwise.
  error: string;
  sections: ReleaseSection[];
  created_by: string;
  created_at: string;
  published_at: string | null;
  retired_at: string | null;
}

// POST /api/admin/releases's body.
export interface NewRelease {
  name?: string;
  size: number;
}

export interface NewReleaseReply {
  release: { id: number };
  upload: { key: string; upload_id: string; part_size: number };
}
