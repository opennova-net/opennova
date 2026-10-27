// The products' admin routes (/api/admin/products*, /api/admin/releases*,
// /api/admin/products/<key>/releases, /api/products/<key>/revisions;
// apps/novaworld_web/products, ADRs 0064 and 0069), in the server's
// snake_case. A product is one row: a game (JO, DFX2), one expansion of a
// game (Escalation, jox01), or an app (kind app: the OpenNova runtime). A
// product with a project key keeps its OpenNova Editor project's
// revisions in the service, and each of its releases is built from one; the
// runtime has no project and its releases arrive from CI. Publishing a release
// is what players update to and what hosted servers run (its built tree).
// Times are the service's UTC 'YYYY-MM-DD HH:MM:SS' (utils/format.ts), null
// until set.

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
