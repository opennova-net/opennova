// Public native assets from published releases, shared independently of the
// source project's visibility. Each download includes the asset's dependencies.
export type AssetKind = 'model' | 'animation' | 'animation_map' | 'terrain' | 'texture';

export interface SharedAsset {
  id: number;
  name: string;
  kind: AssetKind;
  project: { key: string; name: string; owner: string | null };
  release: { id: number; version: string };
  size: number;
  file_count: number;
  total_bytes: number;
}

export interface AssetsPage {
  assets: SharedAsset[];
  next_before: number | null;
}

export interface AssetFilters {
  q?: string;
  kind?: AssetKind;
  project?: string;
  before?: number;
}

export type AssetDownload = { state: 'preparing' } | { state: 'ready'; url: string; filename: string };
export type AssetResult<T> = { ok: true; value: T } | { ok: false; error: string };
