// Data sets, trees and releases for the game data admin tests, shaped as
// GET /api/admin/data-sets and GET /api/admin/releases answer them: JO (id 1)
// with a ready tree and a live server, Escalation (id 2) under it with an
// uploading tree, DFX2 (id 3) with no data.

import type { DataSet, DataSetsResponse, Release, Tree } from '../types/dataSets';

export function tree(id: number, overrides: Partial<Tree> = {}): Tree {
  const state = overrides.state ?? 'ready';
  return {
    id,
    state,
    source: 'baseline',
    release_id: null,
    version: '1.7.5.5',
    bytes: 1_250_000_000,
    files: 7,
    missions: 54,
    pinned_by: 0,
    error: '',
    created_by: 'admin root',
    created_at: '2026-10-16 10:00:00',
    ready_at: state === 'ready' ? '2026-10-16 10:12:00' : null,
    ...overrides,
  };
}

export function dataSet(id: number, overrides: Partial<DataSet> = {}): DataSet {
  return {
    id,
    display_name: `Data set ${id}`,
    kind: 'base',
    parent_id: null,
    expansion_code: '',
    title: 'jo',
    game: 'jop_2_consumer',
    updater_login: '',
    command_file: '',
    retail_updater: false,
    current_tree: null,
    baseline_tree_id: null,
    published_release: null,
    servers_live: 0,
    servers_outdated: 0,
    trees: [],
    ...overrides,
  };
}

export const JO_SET: DataSet = dataSet(1, {
  display_name: 'Joint Operations',
  updater_login: 'JOTRus',
  command_file: 'JOPTR.CMD',
  retail_updater: true,
  current_tree: { id: 10, version: '1.7.5.5', ready_at: '2026-10-16 10:12:00' },
  baseline_tree_id: 10,
  published_release: { id: 4, name: 'JO 1.7.5.5', sha256: 'ab12cd34ef56ab12cd34ef56ab12cd34ef56ab12cd34ef56ab12cd34ef56ab12' },
  servers_live: 3,
  servers_outdated: 1,
  trees: [tree(10, { pinned_by: 2 }), tree(11, { state: 'failed', version: '', error: 'version.txt is missing', bytes: 12, files: 1, missions: 0, created_at: '2026-10-15 09:00:00' })],
});

export const ESCALATION_SET: DataSet = dataSet(2, {
  display_name: 'Escalation',
  kind: 'expansion',
  parent_id: 1,
  expansion_code: 'jox01',
  title: '',
  game: '',
  updater_login: 'JOEus',
  command_file: 'JOEPTR.CMD',
  trees: [tree(12, { state: 'uploading', version: '', bytes: 0, files: 0, missions: 0, ready_at: null })],
});

export const DFX2_SET: DataSet = dataSet(3, { display_name: 'Delta Force Xtreme 2', title: 'dfx2', game: 'dfx2_consumer' });

export function dataSetsResponse(overrides: Partial<DataSetsResponse> = {}): DataSetsResponse {
  return {
    store: { kind: 'local' },
    titles: ['jo', 'dfx2'],
    games: [
      { slug: 'jop_2_consumer', display_name: 'Joint Operations' },
      { slug: 'dfx2_consumer', display_name: 'Delta Force Xtreme 2' },
      { slug: 'opennova_0_consumer', display_name: 'OpenNova' },
    ],
    data_sets: [JO_SET, ESCALATION_SET, DFX2_SET],
    ...overrides,
  };
}

export function release(id: number, overrides: Partial<Release> = {}): Release {
  const status = overrides.status ?? 'published';
  return {
    id,
    status,
    name: `JO 1.7.5.${id}`,
    sha256: 'ab12cd34ef56ab12cd34ef56ab12cd34ef56ab12cd34ef56ab12cd34ef56ab12',
    bytes: 312_000_000,
    package_product: 'Joint Operations',
    error: '',
    sections: [
      { data_set_id: 1, display_name: 'Joint Operations', expansion: '', command_file: 'JOPTR.CMD', version: `1.7.5.${id}`, tree: status === 'published' ? { id: 10, state: 'ready' } : null },
      { data_set_id: 2, display_name: 'Escalation', expansion: 'jox01', command_file: 'JOEPTR.CMD', version: `1.7.5.${id}`, tree: null },
    ],
    created_by: 'admin root',
    created_at: '2026-10-16 11:00:00',
    published_at: status === 'published' || status === 'retired' ? '2026-10-16 11:30:00' : null,
    retired_at: status === 'retired' ? '2026-10-16 12:00:00' : null,
    ...overrides,
  };
}
