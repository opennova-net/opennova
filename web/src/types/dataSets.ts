// The games' admin routes (/api/admin/data-sets*, /api/admin/releases*,
// /api/admin/content/projects/<key>/releases, /api/content/projects/<key>/revisions;
// apps/novaworld_web/data_sets and content, ADRs 0064 and 0069), in the server's
// snake_case. A game is one data set row: a base game (JO, DFX2), one
// expansion of a base (Escalation, jox01), or the OpenNova runtime (kind
// update). A game with a project key keeps its OpenNova Editor project's
// revisions in the service, and each of its releases is built from one; the
// runtime has no project and its releases arrive from CI. Publishing a release
// is what players update to and what hosted servers run (its built tree).
// Times are the service's UTC 'YYYY-MM-DD HH:MM:SS' (utils/format.ts), null
// until set.

export type DataSetKind = 'base' | 'expansion' | 'update';

export type ProjectVisibility = 'public' | 'private';

// A tree is the internal form of a hosted server's data, built from a
// published release: building while the data program applies it, then ready,
// or failed with its error; pruned once its objects are gone.
export type TreeState = 'building' | 'ready' | 'failed' | 'pruned';

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
  // An expansion's base game; null otherwise.
  parent_id: number | null;
  // An expansion's /exp code ('jox01'); '' otherwise.
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
  // Its project's key (1 to 16 of A-Z, 0-9 and _), who may get it, and its
  // newest revision (0 before the first push); key and visibility are null for
  // a game with no project (the runtime).
  key: string | null;
  visibility: ProjectVisibility | null;
  head: number;
  // What new servers start on; null until a release's tree is ready.
  current_tree: CurrentTree | null;
  published_release: PublishedRelease | null;
  // Live servers running it, and how many of them run on an older tree.
  servers_live: number;
  servers_outdated: number;
}

export interface LobbyOption {
  slug: string;
  display_name: string;
}

// GET /api/admin/data-sets.
export interface DataSetsResponse {
  // The titles a base may speak.
  titles: string[];
  // The lobbies a base may list in (the reply's `games`).
  games: LobbyOption[];
  data_sets: DataSet[];
}

// POST /api/admin/data-sets's body: the game and, with key and visibility,
// its project.
export interface NewDataSet {
  display_name: string;
  kind: 'base' | 'expansion';
  parent_id?: number;
  expansion_code?: string;
  title?: string;
  game?: string;
  updater_login?: string;
  command_file?: string;
  retail_updater?: boolean;
  key?: string;
  visibility?: ProjectVisibility;
}

// PATCH /api/admin/data-sets/<id>'s body: any of these (a key renames the
// project).
export interface DataSetPatch {
  display_name?: string;
  game?: string;
  updater_login?: string;
  command_file?: string;
  retail_updater?: boolean;
  key?: string;
  visibility?: ProjectVisibility;
}

// One revision of a game's project, as GET /api/content/projects/<key>/revisions
// lists it.
export interface ProjectRevision {
  number: number;
  // Who pushed it.
  author: string;
  message: string;
  manifest_sha256: string;
  file_count: number;
  bytes: number;
  created_at: string;
}

// building (the data program packs a revision) -> validating -> draft |
// invalid; draft -> published -> retired. The runtime's releases arrive by
// upload (uploading -> validating) from the Publish workflow.
export type ReleaseStatus = 'uploading' | 'building' | 'validating' | 'invalid' | 'draft' | 'published' | 'retired';

// A built release's source: its game's project key and the revision built
// (an expansion's on its base game's revision).
export interface ReleaseSource {
  project: string;
  revision: number;
  base_revision: number | null;
}

// One section of a release: one product's update, mapped to a game by its
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
  // Its game; null while an upload is not yet read.
  data_set_id: number | null;
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
  // A built release's source, null for an upload; its version; what the build's
  // tools said ('' for an upload).
  source: ReleaseSource | null;
  version: string;
  build_log: string;
}
