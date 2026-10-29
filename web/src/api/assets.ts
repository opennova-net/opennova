import { isAxiosError } from 'axios';
import { apiClient } from './client';
import type { AssetDownload, AssetFilters, AssetKind, AssetResult, AssetsPage, SharedAsset } from '../types/assets';

export const ASSETS_PAGE_SIZE = 50;
export const ASSET_KINDS: ReadonlyArray<{ value: AssetKind; label: string }> = [
  { value: 'model', label: 'Models (.3di)' },
  { value: 'animation', label: 'Animations (.bad)' },
  { value: 'animation_map', label: 'Animation maps (.adm)' },
  { value: 'terrain', label: 'Terrains (.trn)' },
  { value: 'texture', label: 'Textures' },
];

const fields = (value: unknown): value is Record<string, unknown> => typeof value === 'object' && value !== null && !Array.isArray(value);
const count = (value: unknown): value is number => Number.isSafeInteger(value) && (value as number) >= 0;
const id = (value: unknown): value is number => count(value) && value > 0;

export function isAssetKind(value: unknown): value is AssetKind {
  return ASSET_KINDS.some((kind) => kind.value === value);
}

function pickAsset(value: unknown): SharedAsset | null {
  if (!fields(value) || !id(value.id) || typeof value.name !== 'string' || !value.name || !isAssetKind(value.kind)) return null;
  const project = value.project;
  const release = value.release;
  if (!fields(project) || typeof project.key !== 'string' || !project.key || typeof project.name !== 'string'
    || !(project.owner === null || typeof project.owner === 'string')) return null;
  if (!fields(release) || !id(release.id) || typeof release.version !== 'string') return null;
  if (!count(value.size) || !id(value.file_count) || !count(value.total_bytes)) return null;
  return {
    id: value.id, name: value.name, kind: value.kind,
    project: { key: project.key, name: project.name, owner: project.owner },
    release: { id: release.id, version: release.version },
    size: value.size, file_count: value.file_count, total_bytes: value.total_bytes,
  };
}

function failure(error: unknown, fallback: string): AssetResult<never> {
  const status = isAxiosError(error) ? error.response?.status : undefined;
  return { ok: false, error: status === 404 ? 'This asset is no longer shared. Refresh the library.'
    : status === 429 ? 'Too many requests. Wait a moment and try again.'
      : fallback };
}

// The public catalog is already filtered to downloadable bundles; this route
// never fetches a source project's manifest or requires its visibility to be public.
export async function fetchAssets(filters: AssetFilters, signal?: AbortSignal): Promise<AssetResult<AssetsPage>> {
  const error = 'The asset library could not be read. Try again.';
  try {
    const { data } = await apiClient.get<unknown>('/assets', { params: { ...filters, limit: ASSETS_PAGE_SIZE }, signal });
    if (!fields(data) || !Array.isArray(data.assets) || !(data.next_before === null || id(data.next_before))) return { ok: false, error };
    const assets = data.assets.map(pickAsset);
    if (assets.some((asset) => asset === null)) return { ok: false, error };
    return { ok: true, value: { assets: assets as SharedAsset[], next_before: data.next_before } };
  } catch (cause) {
    return failure(cause, error);
  }
}

// The store's HTTPS signed link, or the same-origin route used by a local
// store. Refuse active URL schemes before a response becomes a download link.
function isDownloadUrl(value: unknown): value is string {
  if (typeof value !== 'string') return false;
  if (value.startsWith('/api/') && !value.includes('\\')) return true;
  try {
    const url = new URL(value);
    return url.protocol === 'https:' && !url.username && !url.password;
  } catch {
    return false;
  }
}

export async function prepareAssetDownload(assetId: number, signal?: AbortSignal, retry = false): Promise<AssetResult<AssetDownload>> {
  const error = 'The download could not be prepared. Try again.';
  try {
    const { data, status } = await apiClient.post<unknown>(`/assets/${assetId}/download`, retry ? { retry: true } : {}, { signal });
    if (fields(data)) {
      if (status === 202 && data.state === 'preparing') return { ok: true, value: { state: 'preparing' } };
      if (status === 200 && data.state === 'ready' && isDownloadUrl(data.url) && typeof data.filename === 'string' && data.filename) {
        return { ok: true, value: { state: 'ready', url: data.url, filename: data.filename } };
      }
    }
    return { ok: false, error };
  } catch (cause) {
    return failure(cause, error);
  }
}
