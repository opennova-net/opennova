import { AxiosError, AxiosHeaders, type AxiosResponse } from 'axios';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import { adminClient, apiClient } from './client';
import {
  REVISIONS_PAGE,
  buildRelease,
  createProduct,
  deleteProduct,
  deleteRelease,
  fetchProducts,
  fetchReleases,
  fetchRevisions,
  publishRelease,
  retireRelease,
  updateAllServers,
  updateProduct,
} from './products';
import { product, productsResponse, release, revision, runtimeRelease } from '../test/productFixtures';

vi.mock('./client', () => ({
  apiClient: { get: vi.fn() },
  adminClient: { get: vi.fn(), post: vi.fn(), patch: vi.fn(), delete: vi.fn() },
}));

const get = vi.mocked(adminClient.get);
const post = vi.mocked(adminClient.post);
const patch = vi.mocked(adminClient.patch);
const del = vi.mocked(adminClient.delete);
const publicGet = vi.mocked(apiClient.get);

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

describe('the products', () => {
  beforeEach(() => vi.clearAllMocks());

  it('reads the titles, the lobbies and each product with its project', async () => {
    const body = productsResponse();
    get.mockResolvedValue(reply({ store: { kind: 'local' }, ...body }));
    const result = await fetchProducts();
    expect(get).toHaveBeenCalledWith('/products', expect.anything());
    expect(result).toEqual({ ok: true, value: body });
  });

  it('fills what a product leaves out, the runtime among them, and refuses one it cannot read', async () => {
    get.mockResolvedValue(
      reply({
        titles: ['jo'],
        games: [{ slug: 'jop_2_consumer', display_name: 'Joint Operations' }],
        products: [
          { id: 4, display_name: 'OP: Acid Gambit', kind: 'game', title: 'jo', lobby: 'acidgambit_0_consumer', key: 'ACIDGAMBIT', visibility: 'public', head: 14 },
          { id: 9, display_name: 'OpenNova runtime', kind: 'app', key: 'ONRUNTIME', visibility: null, head: 0, current_tree: null, published_release: null },
        ],
      }),
    );
    const result = await fetchProducts();
    expect(result.ok).toBe(true);
    if (!result.ok) return;
    const [game, runtime] = result.value.products;
    expect([game.key, game.visibility, game.head, game.current_tree, game.published_release]).toEqual(['ACIDGAMBIT', 'public', 14, null, null]);
    expect([runtime.kind, runtime.key, runtime.visibility, runtime.head]).toEqual(['app', 'ONRUNTIME', null, 0]);

    get.mockResolvedValue(reply({ titles: [], games: [], products: [{ id: 'x' }] }));
    const bad = await fetchProducts();
    expect(bad.ok).toBe(false);
    if (!bad.ok) expect(bad.error.code).toBe('unknown');
  });

  it('creates a game with its project, changes it, renames its key and deletes it', async () => {
    post.mockResolvedValue(reply({ product: product(5, { display_name: 'Escalation', kind: 'expansion', parent_id: 1, expansion_code: 'jox01', key: 'JOX01' }) }, 201));
    const body = {
      display_name: 'Escalation',
      kind: 'expansion' as const,
      parent_id: 1,
      expansion_code: 'jox01',
      updater_login: 'JOEus',
      command_file: 'JOEPTR.CMD',
      retail: false,
      key: 'JOX01',
      visibility: 'private' as const,
    };
    const created = await createProduct(body);
    expect(post).toHaveBeenCalledWith('/products', body);
    expect(created.ok && created.value.key).toBe('JOX01');

    patch.mockResolvedValue(reply({ product: product(1, { display_name: 'JO', key: 'JO2', visibility: 'public' }) }));
    const changed = await updateProduct(1, { key: 'JO2', visibility: 'public' });
    expect(patch).toHaveBeenCalledWith('/products/1', { key: 'JO2', visibility: 'public' });
    expect(changed.ok && [changed.value.key, changed.value.visibility]).toEqual(['JO2', 'public']);

    del.mockResolvedValue(reply(null, 204));
    expect(await deleteProduct(1)).toEqual({ ok: true, value: true });
    expect(del).toHaveBeenCalledWith('/products/1');
  });

  it('maps the refusals to words, never the server text', async () => {
    const cases: [number, string, string][] = [
      [409, 'duplicate_expansion', 'expansion with that code'],
      [409, 'duplicate_login', 'updater login'],
      [409, 'duplicate_key', 'already has that key'],
      [409, 'no_such_parent', 'no longer exists'],
    ];
    for (const [status, code, text] of cases) {
      post.mockRejectedValueOnce(refusal(status, { error: code, message: 'raw server text' }));
      const refused = await createProduct({ display_name: 'x', kind: 'expansion', parent_id: 1, expansion_code: 'jox01', key: 'X', visibility: 'private' });
      expect(!refused.ok && refused.error.code).toBe(code);
      expect(!refused.ok && refused.error.message).toContain(text);
      expect(!refused.ok && refused.error.message).not.toContain('raw server text');
    }

    del.mockRejectedValueOnce(refusal(409, { error: 'has_expansion' }));
    const base = await deleteProduct(1);
    expect(!base.ok && base.error.message).toBe('An expansion builds on that game. Delete the expansion first.');
    del.mockRejectedValueOnce(refusal(409, { error: 'in_use' }));
    const inUse = await deleteProduct(1);
    expect(!inUse.ok && inUse.error.message).toContain('A live server runs that game');
  });

  it('asks every outdated server to move, of one game or all', async () => {
    post.mockResolvedValue(reply({ updating: 3 }, 202));
    expect(await updateAllServers(1)).toEqual({ ok: true, value: 3 });
    expect(post).toHaveBeenLastCalledWith('/hosted/update-all', { product_id: 1 });
    expect(await updateAllServers()).toEqual({ ok: true, value: 3 });
    expect(post).toHaveBeenLastCalledWith('/hosted/update-all', {});
  });

  it("reads a project's history a page at a time", async () => {
    publicGet.mockResolvedValue(reply({ revisions: [revision(2), revision(1)] }));
    const read = await fetchRevisions('JO', 3);
    expect(read).toEqual({ ok: true, value: [revision(2), revision(1)] });
    expect(publicGet).toHaveBeenCalledWith('/products/JO/revisions', expect.objectContaining({ params: { limit: REVISIONS_PAGE, before: 3 } }));
    await fetchRevisions('JO');
    expect(publicGet).toHaveBeenLastCalledWith('/products/JO/revisions', expect.objectContaining({ params: { limit: REVISIONS_PAGE } }));
    publicGet.mockResolvedValue(reply({ revisions: [{ number: 0 }] }));
    expect((await fetchRevisions('JO')).ok).toBe(false);
  });
});

describe('the releases', () => {
  beforeEach(() => vi.clearAllMocks());

  it('lists them with their game, source and build log', async () => {
    const list = [
      release(3, { status: 'building', sections: [], build_log: '$ opennova-project export\n', bytes: 0, sha256: '' }),
      release(2, { status: 'draft' }),
      runtimeRelease(7),
    ];
    get.mockResolvedValue(reply({ releases: list }));
    const result = await fetchReleases();
    expect(get).toHaveBeenCalledWith('/releases', expect.anything());
    expect(result).toEqual({ ok: true, value: list });
    // An upload not yet read names no game.
    get.mockResolvedValue(reply({ releases: [{ ...runtimeRelease(8, { status: 'uploading', sections: [] }), product_id: null }] }));
    const uploading = await fetchReleases();
    expect(uploading.ok && uploading.value[0].product_id).toBeNull();
    get.mockResolvedValue(reply({ releases: [{ id: 1 }] }));
    expect((await fetchReleases()).ok).toBe(false);
  });

  it('builds one from a revision, and says why one is not built', async () => {
    post.mockResolvedValue(reply({ release: release(9, { status: 'building', sections: [] }) }, 201));
    const built = await buildRelease('JO', { version: '1.7.5.9', revision: 2 });
    expect(post).toHaveBeenCalledWith('/products/JO/releases', { version: '1.7.5.9', revision: 2 });
    expect(built.ok && built.value.status).toBe('building');
    post.mockRejectedValue(refusal(400, { error: 'no_revision' }));
    const none = await buildRelease('JO', { version: '1.7.5.9', revision: 5 });
    expect(!none.ok && none.error.message).toContain('no such revision');
  });

  it('publishes, retires and deletes, each with its refusal', async () => {
    post.mockResolvedValue(reply({ release: release(7, { status: 'published' }) }, 202));
    const published = await publishRelease(7);
    expect(post).toHaveBeenLastCalledWith('/releases/7/publish');
    expect(published.ok && published.value.status).toBe('published');
    post.mockRejectedValue(refusal(409, { error: 'not_draft' }));
    const notDraft = await publishRelease(7);
    expect(!notDraft.ok && notDraft.error.message).toContain('draft');

    post.mockResolvedValue(reply({ release: release(7, { status: 'retired' }) }, 202));
    expect((await retireRelease(7)).ok).toBe(true);
    expect(post).toHaveBeenLastCalledWith('/releases/7/retire');
    post.mockRejectedValue(refusal(409, { error: 'not_published' }));
    const notPublished = await retireRelease(7);
    expect(!notPublished.ok && notPublished.error.code).toBe('not_published');

    del.mockResolvedValue(reply(null, 204));
    expect((await deleteRelease(7)).ok).toBe(true);
    expect(del).toHaveBeenCalledWith('/releases/7');
    const refusals: [string, string][] = [
      ['published', 'Retire it first'],
      ['building', 'building that release'],
      ['validating', 'checking that release'],
    ];
    for (const [code, text] of refusals) {
      del.mockRejectedValueOnce(refusal(409, { error: code }));
      const kept = await deleteRelease(7);
      expect(!kept.ok && kept.error.message).toContain(text);
    }
  });
});
