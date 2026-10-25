// The content store's routes (apps/novaworld_web/content/content_routes.h, ADR 0069), the admin's
// half: the projects (GET /api/content/projects, every one for an admin's session), one project's
// history (GET /api/content/projects/<key>/revisions), and the projects made, changed and deleted
// (POST, PATCH and DELETE /api/admin/content/projects). The editor pushes and pulls the files; this
// page never moves one. Each answers an AdminResult (admin.ts).

import { adminClient, apiClient } from './client';
import { adminCall, type AdminResult } from './admin';
import type { ContentProject, ContentRevision, ContentVisibility, NewContentProject } from '../types/content';

// The most revisions one read of a history asks for (the routes' page is 200 at most).
export const REVISIONS_PAGE = 50;

const isCount = (v: unknown): v is number => Number.isInteger(v) && (v as number) >= 0;
const isVisibility = (v: unknown): v is ContentVisibility => v === 'public' || v === 'private';

// A project, or null when malformed.
export function parseContentProject(value: unknown): ContentProject | null {
  const v = value as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null) return null;
  if (typeof v.key !== 'string' || typeof v.title !== 'string' || !isVisibility(v.visibility)) return null;
  if (typeof v.retail !== 'boolean' || !isCount(v.head) || typeof v.can_push !== 'boolean') return null;
  const storage = v.storage as Record<string, unknown> | null | undefined;
  return {
    key: v.key,
    title: v.title,
    visibility: v.visibility,
    retail: v.retail,
    base: typeof v.base === 'string' ? v.base : null,
    data_set_id: isCount(v.data_set_id) ? v.data_set_id : null,
    head: v.head,
    can_push: v.can_push,
    storage:
      storage && isCount(storage.files) && isCount(storage.bytes) ? { files: storage.files, bytes: storage.bytes } : null,
  };
}

function parseRevision(value: unknown): ContentRevision | null {
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

function every<T>(list: unknown, parse: (value: unknown) => T | null): T[] | null {
  if (!Array.isArray(list)) return null;
  const out = list.map(parse);
  return out.some((item) => item === null) ? null : (out as T[]);
}

function pickProject(data: unknown): ContentProject | null {
  return parseContentProject((data as { project?: unknown } | null)?.project);
}

// GET /api/content/projects: with an admin's session, the private projects too.
export function fetchContentProjects(signal?: AbortSignal): Promise<AdminResult<ContentProject[]>> {
  return adminCall(
    () => apiClient.get<unknown>('/content/projects', { signal }),
    (data) => every((data as { projects?: unknown } | null)?.projects, parseContentProject),
  );
}

// GET /api/content/projects/<key>/revisions?limit=&before=: its history, newest first, the page
// before revision `before` (none: from the head).
export function fetchContentRevisions(
  key: string,
  before?: number,
  signal?: AbortSignal,
): Promise<AdminResult<ContentRevision[]>> {
  const params: Record<string, number> = { limit: REVISIONS_PAGE };
  if (before) params.before = before;
  return adminCall(
    () => apiClient.get<unknown>(`/content/projects/${encodeURIComponent(key)}/revisions`, { params, signal }),
    (data) => every((data as { revisions?: unknown } | null)?.revisions, parseRevision),
  );
}

// POST /api/admin/content/projects: a new project (201); 400 invalid_field, no_such_data_set,
// no_such_base; 409 duplicate_key, data_set_taken.
export function createContentProject(project: NewContentProject): Promise<AdminResult<ContentProject>> {
  return adminCall(() => adminClient.post<unknown>('/content/projects', project), pickProject);
}

// PATCH /api/admin/content/projects/<key> {title?, visibility?}; 404 not_found.
export function updateContentProject(
  key: string,
  change: { title?: string; visibility?: ContentVisibility },
): Promise<AdminResult<ContentProject>> {
  return adminCall(() => adminClient.patch<unknown>(`/content/projects/${encodeURIComponent(key)}`, change), pickProject);
}

// DELETE /api/admin/content/projects/<key>: the project, its history and its files gone (204);
// 409 has_expansion while an expansion's project builds on it.
export function deleteContentProject(key: string): Promise<AdminResult<true>> {
  return adminCall(
    () => adminClient.delete<unknown>(`/content/projects/${encodeURIComponent(key)}`),
    () => true,
  );
}
