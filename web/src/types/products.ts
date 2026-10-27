// The products' admin routes (/api/admin/products*, /api/admin/releases*)
// and the projects' player routes (/api/products*, a project's releases
// /api/products/<key>/releases*, /api/me/projects; apps/novaworld_web/products,
// ADRs 0064, 0069 and 0074), in the server's snake_case. A product is one row:
// a game (JO, DFX2), one expansion of a game (Escalation, jox01), or an app
// (kind app: the OpenNova runtime). A product with a project key keeps its
// OpenNova Editor project's revisions in the service, and each of its releases
// is built from one; the runtime has no project and its releases arrive from
// CI. Publishing a release is what players update to and what hosted servers
// run (its built tree). A product is the service's own (an admin's) or a
// player's: any account may make its own games and mods (expansions), push
// them, build their releases and submit them, which an admin approves before
// they are published. Times are the service's UTC 'YYYY-MM-DD HH:MM:SS'
// (utils/format.ts), null until set.

export type ProductKind = 'game' | 'expansion' | 'app';

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

export interface Product {
  id: number;
  display_name: string;
  kind: ProductKind;
  // An expansion's base game; null otherwise.
  parent_id: number | null;
  // An expansion's /exp code ('jox01'); '' otherwise.
  expansion_code: string;
  // A game's title ('jo', 'dfx2'); '' for an expansion (it speaks its parent's).
  title: string;
  // A game's games slug, the lobby it lists in; '' for an expansion.
  lobby: string;
  // The updater's login and command file, both '' when the update service
  // does not serve it.
  updater_login: string;
  command_file: string;
  // SERVER.LST names NovaLogic's ftp.novaworld.com rather than this service's
  // update host.
  retail: boolean;
  // Its key (1 to 16 of A-Z, 0-9 and _: its project's, a game's), who may get
  // its project, and its newest revision (0 before the first push); visibility
  // is null for an app, which has no project.
  key: string;
  visibility: ProjectVisibility | null;
  head: number;
  // What new servers start on; null until a release's tree is ready.
  current_tree: CurrentTree | null;
  published_release: PublishedRelease | null;
  // Live servers running it, and how many of them run on an older tree.
  servers_live: number;
  servers_outdated: number;
  // The username and id of the player whose project it is; null for the
  // service's own.
  owner: string | null;
  owner_id: number | null;
}

export interface LobbyOption {
  slug: string;
  display_name: string;
}

// GET /api/admin/products.
export interface ProductsResponse {
  // The titles a game may speak.
  titles: string[];
  // The lobbies a game may list in (the reply's `games`).
  games: LobbyOption[];
  products: Product[];
}

// POST /api/admin/products's body: the game and, with key and visibility,
// its project.
export interface NewProduct {
  display_name: string;
  kind: 'game' | 'expansion';
  parent_id?: number;
  expansion_code?: string;
  title?: string;
  lobby?: string;
  updater_login?: string;
  command_file?: string;
  retail?: boolean;
  key?: string;
  visibility?: ProjectVisibility;
}

// PATCH /api/admin/products/<id>'s body: any of these (a key renames the
// project).
export interface ProductPatch {
  display_name?: string;
  lobby?: string;
  updater_login?: string;
  command_file?: string;
  retail?: boolean;
  key?: string;
  visibility?: ProjectVisibility;
}

// A project as the player routes answer it (GET /api/products, GET
// /api/me/projects, POST and PATCH /api/products...): a game's or an
// expansion's, by its key. What the caller may do with it comes with it.
export interface Project {
  key: string;
  // Its name (its product's display name).
  title: string;
  kind: 'game' | 'expansion';
  // The game it runs as ('jo', 'dfx2'): a game's title, an expansion's base
  // game's.
  game: string;
  // An expansion's short name ('jox01'); '' for a game.
  expansion_code: string;
  visibility: ProjectVisibility;
  retail: boolean;
  // An expansion's base game's key; null for a game.
  base: string | null;
  product_id: number;
  // Its newest revision; 0 before the first push.
  head: number;
  // The username of the player whose project it is; null for the service's own.
  owner: string | null;
  // The caller may push to it; may rename it, change who gets it and delete it;
  // may build, publish, retire and delete its releases.
  can_push: boolean;
  can_manage: boolean;
  can_release: boolean;
  // Its files in the store, for its owner and admins; null otherwise.
  storage: ProjectStorage | null;
  // Its published release: what players update to; null with none.
  release: ProjectRelease | null;
}

export interface ProjectRelease {
  id: number;
  version: string;
}

export interface ProjectStorage {
  files: number;
  bytes: number;
}

// A game a mod may be for, as GET /api/me/projects lists it: its key, its
// name (its product's display name) and the game it runs as ('jo').
export interface ProjectGame {
  key: string;
  title: string;
  game: string;
}

// GET /api/me/projects: the account's own projects, how many it has and the
// bytes they store against the most it may (its projects' files and its live
// releases' packages, `release_bytes` the packages' part), and what a new one
// may be: a mod of one of `games` (the service's games and the account's
// own), or a game of its own running as one of `titles` ('jo', 'dfx2').
export interface MyProjects {
  projects: Project[];
  usage: { projects: number; bytes: number; release_bytes: number };
  limits: { projects: number; bytes: number };
  games: ProjectGame[];
  titles: string[];
}

// POST /api/products's body: a project of the caller's own. A game names the
// game it runs as; an expansion (a mod) names its base game's key and its
// short name.
export interface NewProject {
  key: string;
  display_name: string;
  kind: 'game' | 'expansion';
  game?: string;
  base?: string;
  expansion_code?: string;
  visibility: ProjectVisibility;
}

// PATCH /api/products/<key>'s body: either or both.
export interface ProjectPatch {
  display_name?: string;
  visibility?: ProjectVisibility;
}

// One revision of a game's project, as GET /api/products/<key>/revisions
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
// invalid; draft -> published -> retired. A player's own draft goes to
// awaiting_approval when its owner publishes it, and an admin publishes it
// (approves) or sends it back to draft (rejects). The runtime's releases
// arrive by upload (uploading -> validating) from the Publish workflow.
export type ReleaseStatus = 'uploading' | 'building' | 'validating' | 'invalid' | 'draft' | 'awaiting_approval' | 'published' | 'retired';

// A built release's source: its game's project key and the revision built
// (an expansion's on its base game's revision).
export interface ReleaseSource {
  project: string;
  revision: number;
  base_revision: number | null;
}

// One section of a release: one product's update, mapped to a product by its
// command file.
export interface ReleaseSection {
  product_id: number;
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
  // Its product; null while an upload is not yet read.
  product_id: number | null;
  status: ReleaseStatus | (string & {});
  name: string;
  // Hex; '' until validated.
  sha256: string;
  bytes: number;
  // The package's product name.
  package_product: string;
  // Why it is invalid; '' otherwise.
  error: string;
  // What the admin who sent it back to draft said; '' when none.
  review_note: string;
  sections: ReleaseSection[];
  // Who started it: 'user <name>' (a player's own), 'admin <name>', or for an
  // upload 'token' (the admin token).
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
