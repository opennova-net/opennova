import type { SharedAsset } from '../types/assets';

export function sharedAsset(id = 12, overrides: Partial<SharedAsset> = {}): SharedAsset {
  return {
    id,
    name: 'crate.3di',
    kind: 'model',
    project: { key: 'NIGHTOPS', name: 'Night Ops', owner: 'biggy' },
    release: { id: 31, version: '0.2.0' },
    size: 24_000,
    file_count: 3,
    total_bytes: 1_200_000,
    ...overrides,
  };
}
