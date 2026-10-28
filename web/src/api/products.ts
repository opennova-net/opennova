// The products' admin routes (/api/admin/products*, /api/admin/releases*,
// /api/admin/hosted/update-all) and the projects' player routes
// (/api/products*, /api/me/projects: a project's history and its releases, an
// account's own projects; apps/novaworld_web/products, ADRs 0064, 0069 and
// 0074). Like the other admin calls, each admin one answers an AdminResult
// with the server's `error` code mapped to a message (admin.ts); each player
// one answers a ProjectResult, its words a player's. Never the server's text.
// Nothing here uploads: a project's files arrive from the OpenNova Editor, a
// game's releases are built from its project's revisions (by its owner or an
// admin, through the player routes), and the runtime's arrive from the
// Publish workflow.

import { isAxiosError } from 'axios';
import { adminClient, apiClient } from './client';
import { adminCall, type AdminResult } from './admin';
import { formatMemory } from '../utils/format';
import type {
  CurrentTree,
  MyProjects,
  NewProject,
  Product,
  ProductPatch,
  ProductsResponse,
  LobbyOption,
  NewProduct,
  Project,
  ProjectLobby,
  ProjectGame,
  ProjectPatch,
  ProjectRelease,
  ProjectRevision,
  PublishedRelease,
  Release,
  ReleaseSection,
  ReleaseSource,
} from '../types/products';
import { fieldRule } from '../utils/projectsView';

// The most revisions one read of a history asks for (the routes' page is 200 at most).
export const REVISIONS_PAGE = 50;

const isCount = (v: unknown): v is number => Number.isInteger(v) && (v as number) >= 0;
const text = (v: unknown): string => (typeof v === 'string' ? v : '');
const textOrNull = (v: unknown): string | null => (typeof v === 'string' && v !== '' ? v : null);
const idOrNull = (v: unknown): number | null => (isCount(v) ? v : null);

function pickCurrentTree(value: unknown): CurrentTree | null {
  const v = value as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null || !isCount(v.id)) return null;
  return { id: v.id, version: text(v.version), ready_at: textOrNull(v.ready_at) };
}

function pickPublishedRelease(value: unknown): PublishedRelease | null {
  const v = value as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null || !isCount(v.id)) return null;
  return { id: v.id, name: text(v.name), sha256: text(v.sha256) };
}

function pickProduct(value: unknown): Product | null {
  const v = value as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null || !isCount(v.id) || typeof v.display_name !== 'string') return null;
  if (v.kind !== 'game' && v.kind !== 'expansion' && v.kind !== 'app') return null;
  if (typeof v.key !== 'string' || v.key === '') return null;
  return {
    id: v.id,
    display_name: v.display_name,
    kind: v.kind,
    parent_id: idOrNull(v.parent_id),
    expansion_code: text(v.expansion_code),
    title: text(v.title),
    lobby: text(v.lobby),
    lobby_locked: v.lobby_locked === true,
    updater_login: text(v.updater_login),
    command_file: text(v.command_file),
    retail: v.retail === true,
    key: v.key,
    visibility: v.kind !== 'app' && (v.visibility === 'public' || v.visibility === 'private') ? v.visibility : null,
    head: isCount(v.head) ? v.head : 0,
    current_tree: pickCurrentTree(v.current_tree),
    published_release: pickPublishedRelease(v.published_release),
    servers_live: isCount(v.servers_live) ? v.servers_live : 0,
    servers_outdated: isCount(v.servers_outdated) ? v.servers_outdated : 0,
    owner: textOrNull(v.owner),
    owner_id: idOrNull(v.owner_id),
  };
}

function pickLobby(value: unknown): LobbyOption | null {
  const v = value as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null || typeof v.slug !== 'string') return null;
  return { slug: v.slug, display_name: text(v.display_name) || v.slug, title: text(v.title) };
}

function pickProjectLobby(value: unknown): ProjectLobby | null {
  const lobby = pickLobby(value);
  return lobby ? { ...lobby, gate_tag_file: text((value as Record<string, unknown>).gate_tag_file) } : null;
}

function pickProductsResponse(data: unknown): ProductsResponse | null {
  const v = data as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null || !Array.isArray(v.products)) return null;
  const products = v.products.map(pickProduct);
  if (products.some((d) => d === null)) return null;
  return {
    titles: Array.isArray(v.titles) ? v.titles.filter((t): t is string => typeof t === 'string') : [],
    games: Array.isArray(v.games) ? v.games.map(pickLobby).filter((g): g is LobbyOption => g !== null) : [],
    products: products as Product[],
  };
}

function pickSection(value: unknown): ReleaseSection | null {
  const v = value as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null || !isCount(v.product_id)) return null;
  const tree = v.tree as Record<string, unknown> | null | undefined;
  return {
    product_id: v.product_id,
    display_name: text(v.display_name),
    expansion: text(v.expansion),
    command_file: text(v.command_file),
    version: text(v.version),
    tree: tree && isCount(tree.id) ? { id: tree.id, state: text(tree.state) } : null,
  };
}

function pickSource(value: unknown): ReleaseSource | null {
  const v = value as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null || typeof v.project !== 'string' || !isCount(v.revision)) return null;
  return { project: v.project, revision: v.revision, base_revision: isCount(v.base_revision) ? v.base_revision : null };
}

function pickRelease(value: unknown): Release | null {
  const v = value as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null || !isCount(v.id) || typeof v.status !== 'string') return null;
  const sections = Array.isArray(v.sections) ? v.sections.map(pickSection) : [];
  if (sections.some((s) => s === null)) return null;
  return {
    id: v.id,
    product_id: idOrNull(v.product_id),
    status: v.status,
    name: text(v.name),
    sha256: text(v.sha256),
    bytes: isCount(v.bytes) ? v.bytes : 0,
    package_product: text(v.package_product),
    error: text(v.error),
    review_note: text(v.review_note),
    sections: sections as ReleaseSection[],
    created_by: text(v.created_by),
    created_at: text(v.created_at),
    published_at: textOrNull(v.published_at),
    retired_at: textOrNull(v.retired_at),
    source: pickSource(v.source),
    version: text(v.version),
    build_log: text(v.build_log),
  };
}

function pickRevision(value: unknown): ProjectRevision | null {
  const v = value as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null || !isCount(v.number) || v.number === 0) return null;
  if (typeof v.author !== 'string' || typeof v.message !== 'string' || typeof v.manifest_sha256 !== 'string') return null;
  if (!isCount(v.file_count) || !isCount(v.bytes) || typeof v.created_at !== 'string') return null;
  return {
    number: v.number,
    author: v.author,
    message: v.message,
    manifest_sha256: v.manifest_sha256,
    file_count: v.file_count,
    bytes: v.bytes,
    created_at: v.created_at,
  };
}

function pickProjectRelease(value: unknown): ProjectRelease | null {
  const v = value as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null || !isCount(v.id)) return null;
  return { id: v.id, version: text(v.version) };
}

function pickProject(value: unknown): Project | null {
  const v = value as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null || typeof v.key !== 'string' || v.key === '' || !isCount(v.product_id)) return null;
  if (v.kind !== 'game' && v.kind !== 'expansion') return null;
  const storage = v.storage as Record<string, unknown> | null | undefined;
  return {
    key: v.key,
    title: text(v.title) || v.key,
    kind: v.kind,
    game: text(v.game),
    lobby: pickProjectLobby(v.lobby),
    lobby_locked: v.lobby_locked === true,
    expansion_code: text(v.expansion_code),
    // One the reply does not say is public is taken for private.
    visibility: v.visibility === 'public' ? 'public' : 'private',
    retail: v.retail === true,
    base: textOrNull(v.base),
    product_id: v.product_id,
    head: isCount(v.head) ? v.head : 0,
    owner: textOrNull(v.owner),
    can_push: v.can_push === true,
    can_manage: v.can_manage === true,
    can_release: v.can_release === true,
    storage: storage && isCount(storage.files) && isCount(storage.bytes) ? { files: storage.files, bytes: storage.bytes } : null,
    release: pickProjectRelease(v.release),
  };
}

function pickAmounts(value: unknown): { projects: number; bytes: number } | null {
  const v = value as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null || !isCount(v.projects) || !isCount(v.bytes)) return null;
  return { projects: v.projects, bytes: v.bytes };
}

function pickProjectGame(value: unknown): ProjectGame | null {
  const v = value as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null || typeof v.key !== 'string' || v.key === '') return null;
  return { key: v.key, title: text(v.title) || v.key, game: text(v.game) };
}

// The account's projects and amounts must read right; the games and titles a
// new one may take are read leniently (none, or one malformed, is left out:
// the form then says there is nothing to pick).
function pickMyProjects(data: unknown): MyProjects | null {
  const v = data as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null) return null;
  const projects = every(v.projects, pickProject);
  const usage = pickAmounts(v.usage);
  const limits = pickAmounts(v.limits);
  if (!projects || !usage || !limits) return null;
  const releaseBytes = (v.usage as Record<string, unknown>).release_bytes;
  return {
    projects,
    usage: { ...usage, release_bytes: isCount(releaseBytes) ? releaseBytes : 0 },
    limits,
    games: Array.isArray(v.games) ? v.games.map(pickProjectGame).filter((g): g is ProjectGame => g !== null) : [],
    lobbies: Array.isArray(v.lobbies) ? v.lobbies.map(pickLobby).filter((g): g is LobbyOption => g !== null) : [],
    titles: Array.isArray(v.titles) ? v.titles.filter((t): t is string => typeof t === 'string' && t !== '') : [],
  };
}

function every<T>(list: unknown, pick: (value: unknown) => T | null): T[] | null {
  if (!Array.isArray(list)) return null;
  const out = list.map(pick);
  return out.some((item) => item === null) ? null : (out as T[]);
}

function pickProductReply(data: unknown): Product | null {
  return pickProduct((data as { product?: unknown } | null)?.product);
}

function pickProjectReply(data: unknown): Project | null {
  return pickProject((data as { project?: unknown } | null)?.project);
}

function pickReleaseReply(data: unknown): Release | null {
  return pickRelease((data as { release?: unknown } | null)?.release);
}

const nothing = (): true => true;

// ---------------------------------------------------------------------------
// Products.

// GET /api/admin/products: the titles and lobbies a game may take, and every
// product.
export function fetchProducts(signal?: AbortSignal): Promise<AdminResult<ProductsResponse>> {
  return adminCall(() => adminClient.get<unknown>('/products', { signal }), pickProductsResponse);
}

// POST /api/admin/products: a new game or expansion, with its project when a key and
// a visibility are given (201). 400 invalid_field or missing_field; 409
// duplicate_login, duplicate_expansion, duplicate_key, no_such_parent.
export function createProduct(body: NewProduct): Promise<AdminResult<Product>> {
  return adminCall(() => adminClient.post<unknown>('/products', body), pickProductReply);
}

// PATCH /api/admin/products/<id>: its display name, lobby, updater login and
// command file, retail updater flag, project key (a rename) or visibility.
export function updateProduct(id: number, patch: ProductPatch): Promise<AdminResult<Product>> {
  return adminCall(() => adminClient.patch<unknown>(`/products/${id}`, patch), pickProductReply);
}

// DELETE /api/admin/products/<id>: the product, its project and its trees
// (204); 409 in_use while a live server runs it, has_expansion while an
// expansion builds on it.
export function deleteProduct(id: number): Promise<AdminResult<true>> {
  return adminCall(() => adminClient.delete<unknown>(`/products/${id}`), nothing);
}

// POST /api/admin/hosted/update-all: every live server on an older tree
// restarts on its game's current one (202), of the game `productId` alone when
// given; how many were asked.
export function updateAllServers(productId?: number): Promise<AdminResult<number>> {
  return adminCall(
    () => adminClient.post<unknown>('/hosted/update-all', productId === undefined ? {} : { product_id: productId }),
    (data) => {
      const updating = (data as { updating?: unknown } | null)?.updating;
      return isCount(updating) ? updating : null;
    },
  );
}

// ---------------------------------------------------------------------------
// Projects: the player routes. A project the caller may not read answers 404
// not_found, as one that does not exist.

// The `error` codes these routes answer with, plus `network` (no reply at
// all) and `unknown` (a reply this client does not recognise).
export type ProjectErrorCode =
  | 'unauthorized'
  | 'forbidden'
  | 'csrf_header_required'
  | 'proxy_required'
  | 'invalid_json'
  | 'invalid_field'
  | 'missing_field'
  | 'invalid_query'
  | 'not_found'
  | 'no_such_base'
  | 'lobby_locked'
  | 'duplicate_key'
  | 'duplicate_expansion'
  | 'too_many_projects'
  | 'has_expansion'
  | 'in_use'
  | 'rate_limited'
  | 'db_error'
  // A project's releases (the routes under /api/products/<key>/releases).
  | 'release_in_progress'
  | 'too_many_releases'
  | 'quota_exceeded'
  | 'no_revision'
  | 'not_draft'
  | 'not_published'
  | 'published'
  | 'validating'
  | 'building'
  | 'store_unavailable'
  | 'store_error'
  | 'network'
  | 'unknown';

export interface ProjectError {
  code: ProjectErrorCode;
  // The HTTP status, or null when no reply arrived.
  status: number | null;
  // The field an invalid_field or missing_field names; '' otherwise.
  field: string;
  // What to show the player.
  message: string;
}

export type ProjectResult<T> = { ok: true; value: T } | { ok: false; error: ProjectError };

const KNOWN_PROJECT_CODES: ReadonlySet<string> = new Set<ProjectErrorCode>([
  'unauthorized',
  'forbidden',
  'csrf_header_required',
  'proxy_required',
  'invalid_json',
  'invalid_field',
  'missing_field',
  'invalid_query',
  'not_found',
  'no_such_base',
  'lobby_locked',
  'duplicate_key',
  'duplicate_expansion',
  'too_many_projects',
  'has_expansion',
  'in_use',
  'rate_limited',
  'db_error',
  'release_in_progress',
  'too_many_releases',
  'quota_exceeded',
  'no_revision',
  'not_draft',
  'not_published',
  'published',
  'validating',
  'building',
  'store_unavailable',
  'store_error',
]);

// A quota_exceeded refusal's amounts, as its reply carries them.
interface Quota {
  used: number;
  limit: number;
}

function projectMessageFor(code: ProjectErrorCode, field: string, quota: Quota | null = null): string {
  switch (code) {
    case 'unauthorized':
      return 'Your session has ended. Log in again to manage your projects.';
    case 'forbidden':
      return "Only its owner and NovaWorld's admins may change that project.";
    case 'csrf_header_required':
      return 'The server refused the request as not coming from this site. Reload the page and try again.';
    case 'proxy_required':
      return 'The server refused a request that did not come through this site. Reload the page and try again.';
    case 'invalid_json':
    case 'invalid_query':
      return 'The server could not read the request. Reload the page and try again.';
    case 'invalid_field':
    case 'missing_field':
      return fieldRule(field) ?? 'The server refused one of the values sent. Check them and try again.';
    case 'not_found':
      return 'That project is not on NovaWorld, or it is private.';
    case 'no_such_base':
      return 'The game that mod is for is not on NovaWorld any more. Pick another.';
    case 'lobby_locked':
      return 'The lobby cannot change after the first release build has started.';
    case 'duplicate_key':
      return 'Another project already has that key. Pick another.';
    case 'duplicate_expansion':
      return 'Another mod already uses that short name.';
    case 'too_many_projects':
      return 'You have as many projects as an account may have. Delete one to make another.';
    case 'has_expansion':
      return 'A mod is made for this game. Delete the mod first.';
    case 'in_use':
      return 'A live server runs this game. It can go once its servers stop.';
    case 'rate_limited':
      return 'Too many requests. Try again in a little while.';
    case 'db_error':
      return 'The server hit a database error. Try again shortly.';
    case 'release_in_progress':
      return 'One of your releases is still building or being checked. Start the next once it is done.';
    case 'too_many_releases':
      return 'Your account has started as many releases today as it may. Try again tomorrow.';
    case 'quota_exceeded':
      return quota
        ? `That would take your account past the ${formatMemory(quota.limit)} it may store (${formatMemory(quota.used)} in use). Delete a release or a project you no longer need to make room.`
        : 'That would take your account past what it may store. Delete a release or a project you no longer need to make room.';
    case 'no_revision':
      return "There is no such revision to build from. A mod also needs its game's project to have one pushed.";
    case 'not_draft':
      return 'Only a draft release can be published. The list shows where each one is.';
    case 'not_published':
      return 'That release is not published any more.';
    case 'published':
      return 'A published release cannot be deleted. Retire it first.';
    case 'validating':
      return 'NovaWorld is still checking that release. Wait until it is a draft or invalid.';
    case 'building':
      return 'NovaWorld is still building that release. Wait until it is a draft or invalid.';
    case 'store_unavailable':
    case 'store_error':
      return "NovaWorld's file store is not answering right now. Try again shortly.";
    case 'network':
      return 'Could not reach the server. Check your connection and try again.';
    case 'unknown':
      return 'Something went wrong. Try again shortly.';
  }
}

// Any failure of a project call, as a ProjectError.
export function describeProjectError(err: unknown): ProjectError {
  if (isAxiosError(err)) {
    const response = err.response;
    if (!response) return { code: 'network', status: null, field: '', message: projectMessageFor('network', '') };
    const data = response.data as { error?: unknown; field?: unknown; used?: unknown; limit?: unknown } | undefined;
    const serverCode = typeof data?.error === 'string' ? data.error : '';
    const field = typeof data?.field === 'string' ? data.field : '';
    const used = data?.used;
    const limit = data?.limit;
    const quota = isCount(used) && isCount(limit) ? { used, limit } : null;
    // A conflict over a mod's short name is its duplicate whatever the code: a
    // short name is one mod's across NovaWorld.
    const code: ProjectErrorCode =
      response.status === 409 && field === 'expansion_code'
        ? 'duplicate_expansion'
        : KNOWN_PROJECT_CODES.has(serverCode)
          ? (serverCode as ProjectErrorCode)
          : response.status === 401
            ? 'unauthorized'
            : response.status === 429
              ? 'rate_limited'
              : 'unknown';
    return {
      code,
      status: response.status,
      field,
      message: code === 'unknown' ? `${projectMessageFor('unknown', '')} (HTTP ${response.status})` : projectMessageFor(code, field, quota),
    };
  }
  return { code: 'unknown', status: null, field: '', message: projectMessageFor('unknown', '') };
}

// One project request whose reply `pick` reads (null: malformed).
async function projectCall<T>(
  request: () => Promise<{ status: number; data: unknown }>,
  pick: (data: unknown) => T | null,
): Promise<ProjectResult<T>> {
  try {
    const res = await request();
    const value = pick(res.data);
    if (value === null) return { ok: false, error: { code: 'unknown', status: res.status, field: '', message: projectMessageFor('unknown', '') } };
    return { ok: true, value };
  } catch (err) {
    return { ok: false, error: describeProjectError(err) };
  }
}

const projectPath = (key: string): string => `/products/${encodeURIComponent(key)}`;

// GET /api/me/projects: the account's own projects, with how many it has and
// the bytes they store against the most it may, and the games and titles a
// new one may be for.
export function fetchMyProjects(signal?: AbortSignal): Promise<ProjectResult<MyProjects>> {
  return projectCall(() => apiClient.get<unknown>('/me/projects', { signal }), pickMyProjects);
}

// POST /api/products: a project of the caller's own (201). 400 invalid_field
// (with its field) or missing_field; 403 too_many_projects; 404 no_such_base;
// 409 duplicate_key, duplicate_expansion; 429 rate_limited.
export function createProject(body: NewProject): Promise<ProjectResult<Project>> {
  return projectCall(() => apiClient.post<unknown>('/products', body), pickProjectReply);
}

// PATCH /api/products/<key>: its name or who gets it, by its owner or an
// admin.
export function updateProject(key: string, patch: ProjectPatch): Promise<ProjectResult<Project>> {
  return projectCall(() => apiClient.patch<unknown>(projectPath(key), patch), pickProjectReply);
}

// DELETE /api/products/<key>: the project, its history and its files (204),
// by its owner or an admin; 409 has_expansion while a mod is made for it,
// in_use while a live server runs it.
export function deleteProject(key: string): Promise<ProjectResult<true>> {
  return projectCall(() => apiClient.delete<unknown>(projectPath(key)), nothing);
}

// GET /api/products/<key>/revisions?limit=&before=: a project's history,
// newest first, the page before revision `before` (none: from the head). Its
// owner's and an admin's session read a private one too.
export function fetchRevisions(key: string, before?: number, signal?: AbortSignal): Promise<ProjectResult<ProjectRevision[]>> {
  const params: Record<string, number> = { limit: REVISIONS_PAGE };
  if (before) params.before = before;
  return projectCall(
    () => apiClient.get<unknown>(`${projectPath(key)}/revisions`, { params, signal }),
    (data) => every((data as { revisions?: unknown } | null)?.revisions, pickRevision),
  );
}

// GET /api/products/<key>/releases: a project's releases, newest first, for
// its owner and admins (403 forbidden otherwise).
export function fetchProjectReleases(key: string, signal?: AbortSignal): Promise<ProjectResult<Release[]>> {
  return projectCall(
    () => apiClient.get<unknown>(`${projectPath(key)}/releases`, { signal }),
    (data) => every((data as { releases?: unknown } | null)?.releases, pickRelease),
  );
}

// POST /api/products/<key>/releases {version, revision?}: a release the data
// program builds from the revision (the head when none is named; an
// expansion's on its base game's head), building (201), by the project's
// owner or an admin (of any project). 400 invalid_field, no_revision; 403
// forbidden; 409 release_in_progress (one of the account's releases is
// building or being checked); 413 quota_exceeded; 429 too_many_releases (the
// most a day); 503 store_unavailable. It is a draft to publish once it is
// built and checked.
export function buildRelease(key: string, build: { version: string; revision?: number }): Promise<ProjectResult<Release>> {
  return projectCall(() => apiClient.post<unknown>(`${projectPath(key)}/releases`, build), pickReleaseReply);
}

// POST /api/products/<key>/releases/<id>/publish (202): an owner's draft
// goes to awaiting_approval, for an admin to publish; an admin's draft or
// awaiting release is published: the update service serves it and the data
// program builds its game's new tree. 409 not_draft.
export function publishProjectRelease(key: string, id: number): Promise<ProjectResult<Release>> {
  return projectCall(() => apiClient.post<unknown>(`${projectPath(key)}/releases/${id}/publish`), pickReleaseReply);
}

// POST /api/products/<key>/releases/<id>/retire: a published release retired
// (202): the update service stops serving it. 409 not_published.
export function retireProjectRelease(key: string, id: number): Promise<ProjectResult<Release>> {
  return projectCall(() => apiClient.post<unknown>(`${projectPath(key)}/releases/${id}/retire`), pickReleaseReply);
}

// DELETE /api/products/<key>/releases/<id>: a draft, invalid, retired or
// awaiting (withdrawn) release and its package (204). 409 published,
// validating, building.
export function deleteProjectRelease(key: string, id: number): Promise<ProjectResult<true>> {
  return projectCall(() => apiClient.delete<unknown>(`${projectPath(key)}/releases/${id}`), nothing);
}

// ---------------------------------------------------------------------------
// Releases: the admin's view of every product's (a project's are built
// through the player route above, buildRelease, by an admin too).

// GET /api/admin/releases: every product's, newest first.
export function fetchReleases(signal?: AbortSignal): Promise<AdminResult<Release[]>> {
  return adminCall(
    () => adminClient.get<unknown>('/releases', { signal }),
    (data) => every((data as { releases?: unknown } | null)?.releases, pickRelease),
  );
}

// POST /api/admin/releases/<id>/publish: a draft or awaiting release
// published (202), the latter approved: the update service serves it and the
// data program builds its game's new tree. 409 not_draft.
export function publishRelease(id: number): Promise<AdminResult<Release>> {
  return adminCall(() => adminClient.post<unknown>(`/releases/${id}/publish`), pickReleaseReply);
}

// POST /api/admin/releases/<id>/reject {message?}: an awaiting release sent
// back to its owner as a draft (202), the message its review_note. 409
// not_awaiting.
export function rejectRelease(id: number, message: string): Promise<AdminResult<Release>> {
  return adminCall(() => adminClient.post<unknown>(`/releases/${id}/reject`, message ? { message } : {}), pickReleaseReply);
}

// POST /api/admin/releases/<id>/retire: a published release retired (202):
// the update service stops serving it. 409 not_published.
export function retireRelease(id: number): Promise<AdminResult<Release>> {
  return adminCall(() => adminClient.post<unknown>(`/releases/${id}/retire`), pickReleaseReply);
}

// DELETE /api/admin/releases/<id>: a draft, invalid or retired release and its
// package (204). 409 published, validating, building.
export function deleteRelease(id: number): Promise<AdminResult<true>> {
  return adminCall(() => adminClient.delete<unknown>(`/releases/${id}`), nothing);
}
