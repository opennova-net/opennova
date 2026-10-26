// The games' admin routes (/api/admin/data-sets*, /api/admin/releases*,
// /api/admin/hosted/update-all, a project's release build and history;
// apps/novaworld_web/data_sets and content, ADRs 0064 and 0069). Like the other
// admin calls, each answers an AdminResult with the server's `error` code
// mapped to a message (admin.ts), never the server's text. Nothing here
// uploads: a game's releases are built from its project's revisions, and the
// runtime's arrive from the Publish workflow.

import { adminClient, apiClient } from './client';
import { adminCall, type AdminResult } from './admin';
import type {
  CurrentTree,
  DataSet,
  DataSetPatch,
  DataSetsResponse,
  LobbyOption,
  NewDataSet,
  ProjectRevision,
  PublishedRelease,
  Release,
  ReleaseSection,
  ReleaseSource,
} from '../types/dataSets';

// The most revisions one read of a history asks for (the routes' page is 200 at most).
export const REVISIONS_PAGE = 50;

const isCount = (v: unknown): v is number => Number.isInteger(v) && (v as number) >= 0;
const text = (v: unknown): string => (typeof v === 'string' ? v : '');
const timeOrNull = (v: unknown): string | null => (typeof v === 'string' && v !== '' ? v : null);
const idOrNull = (v: unknown): number | null => (isCount(v) ? v : null);

function pickCurrentTree(value: unknown): CurrentTree | null {
  const v = value as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null || !isCount(v.id)) return null;
  return { id: v.id, version: text(v.version), ready_at: timeOrNull(v.ready_at) };
}

function pickPublishedRelease(value: unknown): PublishedRelease | null {
  const v = value as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null || !isCount(v.id)) return null;
  return { id: v.id, name: text(v.name), sha256: text(v.sha256) };
}

function pickDataSet(value: unknown): DataSet | null {
  const v = value as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null || !isCount(v.id) || typeof v.display_name !== 'string') return null;
  if (v.kind !== 'base' && v.kind !== 'expansion' && v.kind !== 'update') return null;
  const key = typeof v.key === 'string' && v.key !== '' ? v.key : null;
  return {
    id: v.id,
    display_name: v.display_name,
    kind: v.kind,
    parent_id: idOrNull(v.parent_id),
    expansion_code: text(v.expansion_code),
    title: text(v.title),
    game: text(v.game),
    updater_login: text(v.updater_login),
    command_file: text(v.command_file),
    retail_updater: v.retail_updater === true,
    key,
    visibility: key !== null && (v.visibility === 'public' || v.visibility === 'private') ? v.visibility : null,
    head: isCount(v.head) ? v.head : 0,
    current_tree: pickCurrentTree(v.current_tree),
    published_release: pickPublishedRelease(v.published_release),
    servers_live: isCount(v.servers_live) ? v.servers_live : 0,
    servers_outdated: isCount(v.servers_outdated) ? v.servers_outdated : 0,
  };
}

function pickLobby(value: unknown): LobbyOption | null {
  const v = value as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null || typeof v.slug !== 'string') return null;
  return { slug: v.slug, display_name: text(v.display_name) || v.slug };
}

function pickDataSetsResponse(data: unknown): DataSetsResponse | null {
  const v = data as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null || !Array.isArray(v.data_sets)) return null;
  const dataSets = v.data_sets.map(pickDataSet);
  if (dataSets.some((d) => d === null)) return null;
  return {
    titles: Array.isArray(v.titles) ? v.titles.filter((t): t is string => typeof t === 'string') : [],
    games: Array.isArray(v.games) ? v.games.map(pickLobby).filter((g): g is LobbyOption => g !== null) : [],
    data_sets: dataSets as DataSet[],
  };
}

function pickSection(value: unknown): ReleaseSection | null {
  const v = value as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null || !isCount(v.data_set_id)) return null;
  const tree = v.tree as Record<string, unknown> | null | undefined;
  return {
    data_set_id: v.data_set_id,
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
    data_set_id: idOrNull(v.data_set_id),
    status: v.status,
    name: text(v.name),
    sha256: text(v.sha256),
    bytes: isCount(v.bytes) ? v.bytes : 0,
    package_product: text(v.package_product),
    error: text(v.error),
    sections: sections as ReleaseSection[],
    created_by: text(v.created_by),
    created_at: text(v.created_at),
    published_at: timeOrNull(v.published_at),
    retired_at: timeOrNull(v.retired_at),
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

function every<T>(list: unknown, pick: (value: unknown) => T | null): T[] | null {
  if (!Array.isArray(list)) return null;
  const out = list.map(pick);
  return out.some((item) => item === null) ? null : (out as T[]);
}

function pickDataSetReply(data: unknown): DataSet | null {
  return pickDataSet((data as { data_set?: unknown } | null)?.data_set);
}

function pickReleaseReply(data: unknown): Release | null {
  return pickRelease((data as { release?: unknown } | null)?.release);
}

const nothing = (): true => true;

// ---------------------------------------------------------------------------
// Games.

// GET /api/admin/data-sets: the titles and lobbies a base may take, and every
// game.
export function fetchDataSets(signal?: AbortSignal): Promise<AdminResult<DataSetsResponse>> {
  return adminCall(() => adminClient.get<unknown>('/data-sets', { signal }), pickDataSetsResponse);
}

// POST /api/admin/data-sets: a new game, with its project when a key and a
// visibility are given (201). 400 invalid_field or missing_field; 409
// duplicate_login, duplicate_expansion, duplicate_key, no_such_parent.
export function createDataSet(body: NewDataSet): Promise<AdminResult<DataSet>> {
  return adminCall(() => adminClient.post<unknown>('/data-sets', body), pickDataSetReply);
}

// PATCH /api/admin/data-sets/<id>: its display name, lobby, updater login and
// command file, retail updater flag, project key (a rename) or visibility.
export function updateDataSet(id: number, patch: DataSetPatch): Promise<AdminResult<DataSet>> {
  return adminCall(() => adminClient.patch<unknown>(`/data-sets/${id}`, patch), pickDataSetReply);
}

// DELETE /api/admin/data-sets/<id>: the game, its project and its trees
// (204); 409 in_use while a live server runs it, has_expansion while an
// expansion builds on it.
export function deleteDataSet(id: number): Promise<AdminResult<true>> {
  return adminCall(() => adminClient.delete<unknown>(`/data-sets/${id}`), nothing);
}

// POST /api/admin/hosted/update-all: every live server on an older tree
// restarts on its game's current one (202), of game `dataSetId` alone when
// given; how many were asked.
export function updateAllServers(dataSetId?: number): Promise<AdminResult<number>> {
  return adminCall(
    () => adminClient.post<unknown>('/hosted/update-all', dataSetId === undefined ? {} : { data_set_id: dataSetId }),
    (data) => {
      const updating = (data as { updating?: unknown } | null)?.updating;
      return isCount(updating) ? updating : null;
    },
  );
}

// ---------------------------------------------------------------------------
// Projects.

// GET /api/content/projects/<key>/revisions?limit=&before=: a game's project
// history, newest first, the page before revision `before` (none: from the
// head). An admin's session reads a private one too.
export function fetchRevisions(key: string, before?: number, signal?: AbortSignal): Promise<AdminResult<ProjectRevision[]>> {
  const params: Record<string, number> = { limit: REVISIONS_PAGE };
  if (before) params.before = before;
  return adminCall(
    () => apiClient.get<unknown>(`/content/projects/${encodeURIComponent(key)}/revisions`, { params, signal }),
    (data) => every((data as { revisions?: unknown } | null)?.revisions, pickRevision),
  );
}

// ---------------------------------------------------------------------------
// Releases.

// GET /api/admin/releases: every game's, newest first.
export function fetchReleases(signal?: AbortSignal): Promise<AdminResult<Release[]>> {
  return adminCall(
    () => adminClient.get<unknown>('/releases', { signal }),
    (data) => every((data as { releases?: unknown } | null)?.releases, pickRelease),
  );
}

// POST /api/admin/content/projects/<key>/releases {version, revision?}: a
// release the data program builds from the revision (the head when none is
// named; an expansion's on its base game's head), building (201); 400
// invalid_field, no_revision; 503 store_unavailable. It is a draft to publish
// once it is built and checked.
export function buildRelease(key: string, build: { version: string; revision?: number }): Promise<AdminResult<Release>> {
  return adminCall(() => adminClient.post<unknown>(`/content/projects/${encodeURIComponent(key)}/releases`, build), pickReleaseReply);
}

// POST /api/admin/releases/<id>/publish: a draft release published (202): the
// update service serves it and the data program builds its game's new tree.
// 409 not_draft.
export function publishRelease(id: number): Promise<AdminResult<Release>> {
  return adminCall(() => adminClient.post<unknown>(`/releases/${id}/publish`), pickReleaseReply);
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
