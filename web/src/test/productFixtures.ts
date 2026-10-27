// Products, releases and revisions for the products admin tests, shaped as
// GET /api/admin/products, GET /api/admin/releases and a project's history
// answer them: JO (id 1, project JO at r2) with a ready tree and a live
// server, Escalation (id 2, project JOX01) under it with nothing pushed, DFX2
// (id 3, project DFX2) with no tree, the OpenNova runtime (id 9, ONRUNTIME, an
// app with no project).

import type { Product, ProductsResponse, ProjectRevision, Release } from '../types/products';

export function product(id: number, overrides: Partial<Product> = {}): Product {
  return {
    id,
    display_name: `Game ${id}`,
    kind: 'game',
    parent_id: null,
    expansion_code: '',
    title: 'jo',
    lobby: 'jop_2_consumer',
    updater_login: '',
    command_file: '',
    retail: false,
    key: `GAME${id}`,
    visibility: 'private',
    head: 0,
    current_tree: null,
    published_release: null,
    servers_live: 0,
    servers_outdated: 0,
    ...overrides,
  };
}

export const JO_PRODUCT: Product = product(1, {
  display_name: 'Joint Operations',
  updater_login: 'JOTRus',
  command_file: 'JOPTR.CMD',
  retail: true,
  key: 'JO',
  head: 2,
  current_tree: { id: 10, version: '1.7.5.9', ready_at: '2026-10-16 10:12:00' },
  published_release: { id: 9, name: 'JO 1.7.5.9', sha256: 'ab12cd34ef56ab12cd34ef56ab12cd34ef56ab12cd34ef56ab12cd34ef56ab12' },
  servers_live: 3,
  servers_outdated: 1,
});

export const ESCALATION_PRODUCT: Product = product(2, {
  display_name: 'Escalation',
  kind: 'expansion',
  parent_id: 1,
  expansion_code: 'jox01',
  title: '',
  lobby: '',
  updater_login: 'JOEus',
  command_file: 'JOEPTR.CMD',
  key: 'JOX01',
});

export const DFX2_PRODUCT: Product = product(3, { display_name: 'Delta Force Xtreme 2', title: 'dfx2', lobby: 'dfx2_consumer', key: 'DFX2', head: 1 });

export const RUNTIME_PRODUCT: Product = product(9, {
  display_name: 'OpenNova runtime',
  kind: 'app',
  title: '',
  lobby: '',
  updater_login: 'ONRUNTIME',
  command_file: 'ONRUNTIME.CMD',
  key: 'ONRUNTIME',
  visibility: null,
  published_release: { id: 7, name: 'v0.0.21', sha256: 'cd34ef56ab12cd34ef56ab12cd34ef56ab12cd34ef56ab12cd34ef56ab12cd34' },
});

export function productsResponse(overrides: Partial<ProductsResponse> = {}): ProductsResponse {
  return {
    titles: ['jo', 'dfx2'],
    games: [
      { slug: 'jop_2_consumer', display_name: 'Joint Operations' },
      { slug: 'dfx2_consumer', display_name: 'Delta Force Xtreme 2' },
      { slug: 'acidgambit_0_consumer', display_name: 'OpenNova' },
    ],
    products: [JO_PRODUCT, ESCALATION_PRODUCT, DFX2_PRODUCT, RUNTIME_PRODUCT],
    ...overrides,
  };
}

// A release of JO built from its project's r2, published unless told.
export function release(id: number, overrides: Partial<Release> = {}): Release {
  const status = overrides.status ?? 'published';
  return {
    id,
    product_id: 1,
    status,
    name: `JO 1.7.5.${id}`,
    sha256: 'ab12cd34ef56ab12cd34ef56ab12cd34ef56ab12cd34ef56ab12cd34ef56ab12',
    bytes: 312_000_000,
    package_product: 'Joint Operations',
    error: '',
    sections: [
      { product_id: 1, display_name: 'Joint Operations', expansion: '', command_file: 'JOPTR.CMD', version: `1.7.5.${id}`, tree: status === 'published' ? { id: 10, state: 'ready' } : null },
    ],
    created_by: 'admin root',
    created_at: '2026-10-16 11:00:00',
    published_at: status === 'published' || status === 'retired' ? '2026-10-16 11:30:00' : null,
    retired_at: status === 'retired' ? '2026-10-16 12:00:00' : null,
    source: { project: 'JO', revision: 2, base_revision: null },
    version: `1.7.5.${id}`,
    build_log: '',
    ...overrides,
  };
}

// The runtime's release from CI: an upload, no source.
export function runtimeRelease(id: number, overrides: Partial<Release> = {}): Release {
  return release(id, {
    product_id: 9,
    name: 'v0.0.21',
    package_product: 'OpenNova',
    sections: [{ product_id: 9, display_name: 'OpenNova runtime', expansion: '', command_file: 'ONRUNTIME.CMD', version: '0.0.21', tree: null }],
    source: null,
    version: '',
    ...overrides,
  });
}

export function revision(number: number, overrides: Partial<ProjectRevision> = {}): ProjectRevision {
  return {
    number,
    author: 'biggy',
    message: number === 1 ? 'Stock import' : 'weapon tweaks',
    manifest_sha256: 'ab',
    file_count: 1200,
    bytes: 1_200_000_000,
    created_at: '2026-10-25 12:00:00',
    ...overrides,
  };
}
