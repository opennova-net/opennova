// The game data admin routes (/api/admin/data-sets*,
// /api/admin/hosted/update-all; apps/novaworld_web/data_sets, ADR 0064). Like
// the other admin calls, each answers an AdminResult with the server's `error`
// code mapped to a message (admin.ts), never the server's text; the two
// refusals that name what went wrong (invalid_files' missing and unexpected
// names, pinned's server count) add it to the message.

import { isAxiosError } from 'axios';
import { adminClient } from './client';
import { describeAdminError, malformedAdminReply, type AdminError, type AdminResult } from './admin';
import type {
  CurrentTree,
  DataSet,
  DataSetPatch,
  DataSetsResponse,
  GameOption,
  NewDataSet,
  NewTreeReply,
  Tree,
  TreeUpload,
  TreeUploadFile,
  UploadedPart,
} from '../types/dataSets';

const isCount = (v: unknown): v is number => Number.isInteger(v) && (v as number) >= 0;
const text = (v: unknown): string => (typeof v === 'string' ? v : '');
const timeOrNull = (v: unknown): string | null => (typeof v === 'string' && v !== '' ? v : null);
const idOrNull = (v: unknown): number | null => (isCount(v) ? v : null);

function pickTree(value: unknown): Tree | null {
  const v = value as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null || !isCount(v.id) || typeof v.state !== 'string') return null;
  return {
    id: v.id,
    state: v.state,
    version: text(v.version),
    bytes: isCount(v.bytes) ? v.bytes : 0,
    files: isCount(v.files) ? v.files : 0,
    missions: isCount(v.missions) ? v.missions : 0,
    pinned_by: isCount(v.pinned_by) ? v.pinned_by : 0,
    error: text(v.error),
    created_by: text(v.created_by),
    created_at: text(v.created_at),
    ready_at: timeOrNull(v.ready_at),
  };
}

function pickCurrentTree(value: unknown): CurrentTree | null {
  const v = value as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null || !isCount(v.id)) return null;
  return { id: v.id, version: text(v.version), ready_at: timeOrNull(v.ready_at) };
}

function pickDataSet(value: unknown): DataSet | null {
  const v = value as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null || !isCount(v.id) || typeof v.display_name !== 'string') return null;
  if (v.kind !== 'base' && v.kind !== 'expansion') return null;
  const trees = Array.isArray(v.trees) ? v.trees.map(pickTree) : [];
  if (trees.some((t) => t === null)) return null;
  return {
    id: v.id,
    display_name: v.display_name,
    kind: v.kind,
    parent_id: idOrNull(v.parent_id),
    expansion_code: text(v.expansion_code),
    title: text(v.title),
    game: text(v.game),
    current_tree: pickCurrentTree(v.current_tree),
    servers_live: isCount(v.servers_live) ? v.servers_live : 0,
    servers_outdated: isCount(v.servers_outdated) ? v.servers_outdated : 0,
    trees: trees as Tree[],
  };
}

function pickGame(value: unknown): GameOption | null {
  const v = value as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null || typeof v.slug !== 'string') return null;
  return { slug: v.slug, display_name: text(v.display_name) || v.slug };
}

function pickDataSetsResponse(data: unknown): DataSetsResponse | null {
  const v = data as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null || !Array.isArray(v.data_sets)) return null;
  const dataSets = v.data_sets.map(pickDataSet);
  if (dataSets.some((d) => d === null)) return null;
  const store = v.store as Record<string, unknown> | null | undefined;
  const games = Array.isArray(v.games) ? v.games.map(pickGame).filter((g): g is GameOption => g !== null) : [];
  return {
    store: { kind: text(store?.kind) || 'local' },
    titles: Array.isArray(v.titles) ? v.titles.filter((t): t is string => typeof t === 'string') : [],
    games,
    data_sets: dataSets as DataSet[],
  };
}

function pickDataSetReply(data: unknown): DataSet | null {
  return pickDataSet((data as { data_set?: unknown } | null)?.data_set);
}

// "a, b and c".
function listed(names: string[]): string {
  if (names.length <= 1) return names.join('');
  return `${names.slice(0, -1).join(', ')} and ${names[names.length - 1]}`;
}

// A refusal of these routes: the admin's message, with what invalid_files
// and pinned named added.
export function describeDataSetError(err: unknown): AdminError {
  const error = describeAdminError(err);
  if (!isAxiosError(err) || !err.response) return error;
  const data = err.response.data as Record<string, unknown> | undefined;
  if (error.code === 'invalid_files') {
    const names = (key: string) => (Array.isArray(data?.[key]) ? (data[key] as unknown[]).filter((n): n is string => typeof n === 'string') : []);
    const missing = names('missing');
    const unexpected = names('unexpected');
    const parts: string[] = [];
    if (missing.length > 0) parts.push(`Missing: ${listed(missing)}.`);
    if (unexpected.length > 0) parts.push(`Not part of a tree: ${listed(unexpected)}.`);
    if (parts.length > 0) return { ...error, message: `${error.message} ${parts.join(' ')}` };
  }
  if (error.code === 'pinned' && isCount(data?.servers)) {
    const n = data.servers;
    return {
      ...error,
      message: `${n} live server${n === 1 ? ' runs' : 's run'} on that tree, so it stays until ${n === 1 ? 'it updates or stops' : 'they update or stop'}.`,
    };
  }
  return error;
}

// One request whose reply `pick` reads (null: malformed).
async function call<T>(request: () => Promise<{ status: number; data: unknown }>, pick: (data: unknown) => T | null): Promise<AdminResult<T>> {
  try {
    const res = await request();
    const value = pick(res.data);
    if (value === null) return malformedAdminReply(res.status);
    return { ok: true, value };
  } catch (err) {
    return { ok: false, error: describeDataSetError(err) };
  }
}

const nothing = (): void => undefined;

// A POST whose reply body the page does not need.
function post(path: string, body?: object): Promise<AdminResult<void>> {
  return call(() => adminClient.post<unknown>(path, body), nothing);
}

function remove(path: string): Promise<AdminResult<void>> {
  return call(() => adminClient.delete<unknown>(path), nothing);
}

// ---------------------------------------------------------------------------
// Data sets.

// GET /api/admin/data-sets: the store, the titles and lobbies a base may
// take, and every data set with its trees.
export function fetchDataSets(signal?: AbortSignal): Promise<AdminResult<DataSetsResponse>> {
  return call(() => adminClient.get<unknown>('/data-sets', { signal }), pickDataSetsResponse);
}

// POST /api/admin/data-sets: a new data set (201). 400 invalid_field or
// missing_field; 409 duplicate_expansion, no_such_parent.
export function createDataSet(body: NewDataSet): Promise<AdminResult<DataSet>> {
  return call(() => adminClient.post<unknown>('/data-sets', body), pickDataSetReply);
}

// PATCH /api/admin/data-sets/<id>: its display name, or a base's lobby.
export function updateDataSet(id: number, patch: DataSetPatch): Promise<AdminResult<DataSet>> {
  return call(() => adminClient.patch<unknown>(`/data-sets/${id}`, patch), pickDataSetReply);
}

// DELETE /api/admin/data-sets/<id>: the data set and its trees (204); 409
// in_use while a live server runs it or an expansion belongs to it.
export function deleteDataSet(id: number): Promise<AdminResult<void>> {
  return remove(`/data-sets/${id}`);
}

// ---------------------------------------------------------------------------
// Trees: an upload goes POST trees (the files by name and size) -> per file,
// each part PUT through the site (utils/partUpload.ts) -> POST complete per
// file -> POST finalize.

// POST /api/admin/data-sets/<id>/trees: a new uploading tree with one
// multipart upload per file (201). 400 invalid_files (the missing and
// unexpected names), 409 upload_in_flight, 503 store_unavailable.
export function createTree(dataSetId: number, files: TreeUploadFile[]): Promise<AdminResult<NewTreeReply>> {
  return call(
    () => adminClient.post<unknown>(`/data-sets/${dataSetId}/trees`, { files }),
    (data) => {
      const v = data as Record<string, unknown> | null;
      const tree = v?.tree as Record<string, unknown> | null | undefined;
      if (!tree || !isCount(tree.id) || !Array.isArray(v?.uploads)) return null;
      const uploads: TreeUpload[] = [];
      for (const u of v.uploads as (Record<string, unknown> | null)[]) {
        if (typeof u?.name !== 'string' || typeof u.upload_id !== 'string' || !isCount(u.part_size) || u.part_size === 0) return null;
        uploads.push({ name: u.name, key: text(u.key), upload_id: u.upload_id, part_size: u.part_size });
      }
      return { tree: { id: tree.id }, uploads };
    },
  );
}

// PUT /api/admin/data-sets/<id>/trees/<t>/files/<name>/parts/<n>?upload_id=<u>:
// one part of one file, its raw bytes, which web hands to the store; the
// store's ETag, for the complete call. 409 not_uploading, 413 too_large (a
// part over the API's part_size), 502 store_error, 503 store_unavailable.
export function putTreePart(
  dataSetId: number,
  treeId: number,
  name: string,
  uploadId: string,
  partNumber: number,
  blob: Blob,
  signal?: AbortSignal,
): Promise<AdminResult<{ etag: string }>> {
  return call(
    () =>
      adminClient.put<unknown>(`/data-sets/${dataSetId}/trees/${treeId}/files/${encodeURIComponent(name)}/parts/${partNumber}`, blob, {
        params: { upload_id: uploadId },
        headers: { 'Content-Type': 'application/octet-stream' },
        signal,
      }),
    (data) => {
      const etag = (data as { etag?: unknown } | null)?.etag;
      return typeof etag === 'string' && etag !== '' ? { etag } : null;
    },
  );
}

// POST /api/admin/data-sets/<id>/trees/<t>/files/<name>/complete: the file's
// parts joined in the store. 502 store_error.
export function completeTreeFile(
  dataSetId: number,
  treeId: number,
  name: string,
  body: { upload_id: string; parts: UploadedPart[] },
): Promise<AdminResult<void>> {
  return post(`/data-sets/${dataSetId}/trees/${treeId}/files/${encodeURIComponent(name)}/complete`, body);
}

// POST /api/admin/data-sets/<id>/trees/<t>/finalize: every file complete,
// the worker checks and lists the tree (202, state finalizing). 409
// not_uploading.
export function finalizeTree(dataSetId: number, treeId: number): Promise<AdminResult<Tree>> {
  return call(
    () => adminClient.post<unknown>(`/data-sets/${dataSetId}/trees/${treeId}/finalize`),
    (data) => pickTree((data as { tree?: unknown } | null)?.tree),
  );
}

// DELETE /api/admin/data-sets/<id>/trees/<t>: an uploading tree's uploads
// aborted and the row gone (204), or a ready or failed tree pruned (202). 409
// current, pinned (with how many servers).
export function deleteTree(dataSetId: number, treeId: number): Promise<AdminResult<void>> {
  return remove(`/data-sets/${dataSetId}/trees/${treeId}`);
}

// POST /api/admin/hosted/update-all: every live server on an older tree
// restarts on its data set's current one (202), of data set `dataSetId` alone
// when given; how many were asked.
export function updateAllServers(dataSetId?: number): Promise<AdminResult<number>> {
  return call(
    () => adminClient.post<unknown>('/hosted/update-all', dataSetId === undefined ? {} : { data_set_id: dataSetId }),
    (data) => {
      const updating = (data as { updating?: unknown } | null)?.updating;
      return isCount(updating) ? updating : null;
    },
  );
}
