import { AxiosError, AxiosHeaders, type AxiosResponse } from 'axios';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import { adminClient } from './client';
import { completeTreeFile, createDataSet, createTree, deleteDataSet, deleteTree, fetchDataSets, finalizeTree, putTreePart, updateAllServers, updateDataSet } from './dataSets';
import { dataSet, dataSetsResponse, tree } from '../test/dataSetFixtures';

vi.mock('./client', () => ({
  apiClient: { get: vi.fn(), post: vi.fn() },
  adminClient: { get: vi.fn(), post: vi.fn(), put: vi.fn(), patch: vi.fn(), delete: vi.fn() },
}));

const get = vi.mocked(adminClient.get);
const post = vi.mocked(adminClient.post);
const put = vi.mocked(adminClient.put);
const patch = vi.mocked(adminClient.patch);
const del = vi.mocked(adminClient.delete);

function reply(data: unknown, status = 200) {
  return { status, data } as AxiosResponse;
}

// A refusal as axios throws it.
function refusal(status: number, data: unknown): AxiosError {
  const config = { headers: new AxiosHeaders() };
  return new AxiosError('refused', 'ERR_BAD_REQUEST', config, null, {
    status,
    statusText: '',
    data,
    headers: {},
    config,
  } as AxiosResponse);
}

describe('the data sets', () => {
  beforeEach(() => vi.clearAllMocks());

  it('reads the store, the titles, the lobbies and each data set with its trees', async () => {
    const body = dataSetsResponse();
    get.mockResolvedValue(reply(body));
    const result = await fetchDataSets();
    expect(get).toHaveBeenCalledWith('/data-sets', expect.anything());
    expect(result).toEqual({ ok: true, value: body });
  });

  it('fills what a data set leaves null, and refuses one it cannot read', async () => {
    get.mockResolvedValue(
      reply({
        store: { kind: 's3' },
        titles: ['jo'],
        games: [{ slug: 'jop_2_consumer', display_name: 'Joint Operations' }],
        data_sets: [
          {
            id: 4,
            display_name: 'OpenNova',
            kind: 'base',
            parent_id: null,
            expansion_code: '',
            title: 'jo',
            game: 'opennova_0_consumer',
            current_tree: null,
            servers_live: 0,
            servers_outdated: 0,
            trees: [{ id: 9, state: 'uploading', version: '', bytes: 0, files: 0, missions: 0, pinned_by: 0, error: '', created_by: 'admin root', created_at: '2026-10-16 10:00:00', ready_at: '' }],
          },
        ],
      }),
    );
    const result = await fetchDataSets();
    expect(result.ok).toBe(true);
    if (!result.ok) return;
    expect(result.value.data_sets[0].current_tree).toBeNull();
    expect(result.value.data_sets[0].trees[0].ready_at).toBeNull();

    get.mockResolvedValue(reply({ store: { kind: 'local' }, titles: [], games: [], data_sets: [{ id: 'x' }] }));
    const bad = await fetchDataSets();
    expect(bad.ok).toBe(false);
    if (!bad.ok) expect(bad.error.code).toBe('unknown');
  });

  it('creates, changes and deletes a data set', async () => {
    post.mockResolvedValue(reply({ data_set: dataSet(5, { display_name: 'Escalation', kind: 'expansion', parent_id: 1, expansion_code: 'jox01' }) }, 201));
    const created = await createDataSet({ display_name: 'Escalation', kind: 'expansion', parent_id: 1, expansion_code: 'jox01' });
    expect(post).toHaveBeenCalledWith('/data-sets', { display_name: 'Escalation', kind: 'expansion', parent_id: 1, expansion_code: 'jox01' });
    expect(created.ok && created.value.expansion_code).toBe('jox01');

    patch.mockResolvedValue(reply({ data_set: dataSet(1, { display_name: 'JO' }) }));
    const changed = await updateDataSet(1, { display_name: 'JO', game: 'jop_2_consumer' });
    expect(patch).toHaveBeenCalledWith('/data-sets/1', { display_name: 'JO', game: 'jop_2_consumer' });
    expect(changed.ok && changed.value.display_name).toBe('JO');

    del.mockResolvedValue(reply(null, 204));
    expect((await deleteDataSet(1)).ok).toBe(true);
    expect(del).toHaveBeenCalledWith('/data-sets/1');
  });

  it('maps the refusals to words, never the server text', async () => {
    post.mockRejectedValue(refusal(409, { error: 'duplicate_expansion', message: 'raw server text' }));
    const code = await createDataSet({ display_name: 'x', kind: 'expansion', parent_id: 1, expansion_code: 'jox01' });
    expect(!code.ok && code.error.code).toBe('duplicate_expansion');
    expect(!code.ok && code.error.message).toContain('expansion with that code');
    expect(!code.ok && code.error.message).not.toContain('raw server text');

    post.mockRejectedValue(refusal(409, { error: 'no_such_parent' }));
    const parent = await createDataSet({ display_name: 'x', kind: 'expansion', parent_id: 99, expansion_code: 'jox09' });
    expect(!parent.ok && parent.error.code).toBe('no_such_parent');

    del.mockRejectedValue(refusal(409, { error: 'in_use', message: 'a live server runs it' }));
    const inUse = await deleteDataSet(1);
    expect(!inUse.ok && inUse.error.code).toBe('in_use');
    expect(!inUse.ok && inUse.error.message).toContain('in use');
  });
});

describe('the trees', () => {
  beforeEach(() => vi.clearAllMocks());

  it('opens an upload with the files by name and size', async () => {
    post.mockResolvedValue(
      reply(
        {
          tree: { id: 12 },
          uploads: [
            { name: 'resource.pff', key: 'trees/12/resource.pff', upload_id: 'u1', part_size: 33554432 },
            { name: 'localres.pff', key: 'trees/12/localres.pff', upload_id: 'u2', part_size: 33554432 },
          ],
        },
        201,
      ),
    );
    const files = [
      { name: 'resource.pff', size: 1000 },
      { name: 'localres.pff', size: 2000 },
    ];
    const result = await createTree(1, files);
    expect(post).toHaveBeenCalledWith('/data-sets/1/trees', { files });
    expect(result.ok && result.value.tree.id).toBe(12);
    expect(result.ok && result.value.uploads.map((u) => u.upload_id)).toEqual(['u1', 'u2']);

    // An upload with no part size cannot be used.
    post.mockResolvedValue(reply({ tree: { id: 12 }, uploads: [{ name: 'resource.pff', upload_id: 'u1', part_size: 0 }] }, 201));
    expect((await createTree(1, files)).ok).toBe(false);
  });

  it('names the missing and unexpected files of a refused upload', async () => {
    post.mockRejectedValue(refusal(400, { error: 'invalid_files', missing: ['language.pff'], unexpected: ['Jointops.exe', 'readme.txt'] }));
    const result = await createTree(1, [{ name: 'resource.pff', size: 1 }]);
    expect(result.ok).toBe(false);
    if (result.ok) return;
    expect(result.error.code).toBe('invalid_files');
    expect(result.error.message).toContain('Missing: language.pff.');
    expect(result.error.message).toContain('Not part of a tree: Jointops.exe and readme.txt.');

    post.mockRejectedValue(refusal(409, { error: 'upload_in_flight' }));
    const busy = await createTree(1, []);
    expect(!busy.ok && busy.error.code).toBe('upload_in_flight');
    post.mockRejectedValue(refusal(503, { error: 'store_unavailable' }));
    const down = await createTree(1, []);
    expect(!down.ok && down.error.message).toContain('object store');
  });

  it('puts each part through the site as raw bytes, and keeps the ETag it answers', async () => {
    put.mockResolvedValue(reply({ etag: '"e3"' }));
    const blob = new Blob(['abc']);
    const controller = new AbortController();
    const stored = await putTreePart(1, 12, '_NSTMOUT.TXT', 'u1', 3, blob, controller.signal);
    expect(put).toHaveBeenCalledWith('/data-sets/1/trees/12/files/_NSTMOUT.TXT/parts/3', blob, {
      params: { upload_id: 'u1' },
      headers: { 'Content-Type': 'application/octet-stream' },
      signal: controller.signal,
    });
    expect(stored).toEqual({ ok: true, value: { etag: '"e3"' } });

    // Without an ETag the file could not be completed: a malformed reply.
    put.mockResolvedValue(reply({ etag: '' }));
    const noEtag = await putTreePart(1, 12, 'resource.pff', 'u1', 1, blob);
    expect(!noEtag.ok && noEtag.error.code).toBe('unknown');

    put.mockRejectedValue(refusal(413, { error: 'too_large' }));
    const big = await putTreePart(1, 12, 'resource.pff', 'u1', 1, blob);
    expect(!big.ok && big.error.code).toBe('too_large');
    put.mockRejectedValue(refusal(409, { error: 'not_uploading' }));
    const stale = await putTreePart(1, 12, 'resource.pff', 'u1', 1, blob);
    expect(!stale.ok && stale.error.code).toBe('not_uploading');
    put.mockRejectedValue(refusal(502, { error: 'store_error' }));
    const store = await putTreePart(1, 12, 'resource.pff', 'u1', 1, blob);
    expect(!store.ok && store.error.message).toContain('object store');
  });

  it('completes each file by name with its parts, and finalizes', async () => {
    post.mockResolvedValue(reply({ completed: true }));
    const parts = [
      { part_number: 1, etag: '"e1"' },
      { part_number: 2, etag: '"e2"' },
    ];
    expect((await completeTreeFile(1, 12, '_NSTMOUT.TXT', { upload_id: 'u1', parts })).ok).toBe(true);
    expect(post).toHaveBeenLastCalledWith('/data-sets/1/trees/12/files/_NSTMOUT.TXT/complete', { upload_id: 'u1', parts });

    post.mockResolvedValue(reply({ tree: tree(12, { state: 'finalizing' }) }, 202));
    const finalized = await finalizeTree(1, 12);
    expect(post).toHaveBeenLastCalledWith('/data-sets/1/trees/12/finalize');
    expect(finalized.ok && finalized.value.state).toBe('finalizing');

    post.mockRejectedValue(refusal(409, { error: 'not_uploading' }));
    const stale = await finalizeTree(1, 12);
    expect(!stale.ok && stale.error.code).toBe('not_uploading');
    post.mockRejectedValue(refusal(502, { error: 'store_error' }));
    const store = await completeTreeFile(1, 12, 'resource.pff', { upload_id: 'u1', parts });
    expect(!store.ok && store.error.code).toBe('store_error');
  });

  it('deletes a tree, and says why one stays', async () => {
    del.mockResolvedValue(reply(null, 202));
    expect((await deleteTree(1, 12)).ok).toBe(true);
    expect(del).toHaveBeenCalledWith('/data-sets/1/trees/12');

    del.mockRejectedValue(refusal(409, { error: 'pinned', servers: 2 }));
    const pinned = await deleteTree(1, 12);
    expect(!pinned.ok && pinned.error.code).toBe('pinned');
    expect(!pinned.ok && pinned.error.message).toBe('2 live servers run on that tree, so it stays until they update or stop.');
    del.mockRejectedValue(refusal(409, { error: 'pinned', servers: 1 }));
    const one = await deleteTree(1, 12);
    expect(!one.ok && one.error.message).toBe('1 live server runs on that tree, so it stays until it updates or stops.');
    del.mockRejectedValue(refusal(409, { error: 'current' }));
    const current = await deleteTree(1, 12);
    expect(!current.ok && current.error.message).toContain('current data');
  });

  it('asks every outdated server to update, of one data set or all', async () => {
    post.mockResolvedValue(reply({ updating: 3 }, 202));
    expect(await updateAllServers(1)).toEqual({ ok: true, value: 3 });
    expect(post).toHaveBeenLastCalledWith('/hosted/update-all', { data_set_id: 1 });
    expect(await updateAllServers()).toEqual({ ok: true, value: 3 });
    expect(post).toHaveBeenLastCalledWith('/hosted/update-all', {});
  });
});
