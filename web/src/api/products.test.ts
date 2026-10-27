import { AxiosError, AxiosHeaders, type AxiosResponse } from 'axios';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import { adminClient, apiClient } from './client';
import {
  REVISIONS_PAGE,
  buildRelease,
  createProduct,
  createProject,
  deleteProduct,
  deleteProject,
  deleteRelease,
  fetchMyProjects,
  fetchProducts,
  fetchReleases,
  fetchRevisions,
  publishRelease,
  retireRelease,
  updateAllServers,
  updateProduct,
  updateProject,
} from './products';
import { NIGHTOPS_PROJECT, myProjects, product, productsResponse, release, revision, runtimeRelease } from '../test/productFixtures';

vi.mock('./client', () => ({
  apiClient: { get: vi.fn(), post: vi.fn(), patch: vi.fn(), delete: vi.fn() },
  adminClient: { get: vi.fn(), post: vi.fn(), patch: vi.fn(), delete: vi.fn() },
}));

const get = vi.mocked(adminClient.get);
const post = vi.mocked(adminClient.post);
const patch = vi.mocked(adminClient.patch);
const del = vi.mocked(adminClient.delete);
const publicGet = vi.mocked(apiClient.get);
const publicPost = vi.mocked(apiClient.post);
const publicPatch = vi.mocked(apiClient.patch);
const publicDel = vi.mocked(apiClient.delete);

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
          { id: 4, display_name: 'OP: Acid Gambit', kind: 'game', title: 'jo', lobby: 'acidgambit_0_consumer', key: 'ACIDGAMBIT', visibility: 'public', head: 14, owner: null },
          { id: 9, display_name: 'OpenNova runtime', kind: 'app', key: 'ONRUNTIME', visibility: null, head: 0, current_tree: null, published_release: null },
        ],
      }),
    );
    const result = await fetchProducts();
    expect(result.ok).toBe(true);
    if (!result.ok) return;
    const [game, runtime] = result.value.products;
    expect([game.key, game.visibility, game.head, game.current_tree, game.published_release, game.owner]).toEqual(['ACIDGAMBIT', 'public', 14, null, null, null]);
    expect([runtime.kind, runtime.key, runtime.visibility, runtime.head]).toEqual(['app', 'ONRUNTIME', null, 0]);

    get.mockResolvedValue(reply({ titles: [], games: [], products: [{ id: 'x' }] }));
    const bad = await fetchProducts();
    expect(bad.ok).toBe(false);
    if (!bad.ok) expect(bad.error.code).toBe('unknown');
  });

  it("reads a player's product with its owner", async () => {
    get.mockResolvedValue(reply(productsResponse({ products: [product(21, { key: 'NIGHTOPS', kind: 'expansion', parent_id: 1, owner: 'biggy' })] })));
    const result = await fetchProducts();
    expect(result.ok && result.value.products[0].owner).toBe('biggy');
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
    publicGet.mockRejectedValueOnce(refusal(404, { error: 'not_found', message: 'raw server text' }));
    const gone = await fetchRevisions('NIGHTOPS');
    expect(!gone.ok && gone.error.message).toBe('That project is not on NovaWorld, or it is private.');
  });
});

describe('the projects', () => {
  beforeEach(() => vi.clearAllMocks());

  it("reads the account's own projects with their usage and limits, and the games and titles a new one may take", async () => {
    publicGet.mockResolvedValue(reply(myProjects()));
    expect(await fetchMyProjects()).toEqual({ ok: true, value: myProjects() });
    expect(publicGet).toHaveBeenCalledWith('/me/projects', expect.anything());
    publicGet.mockResolvedValue(reply({ projects: [], usage: { projects: 0 } }));
    const bad = await fetchMyProjects();
    expect(!bad.ok && bad.error.code).toBe('unknown');
  });

  it('fills what a project leaves out and refuses one it cannot read', async () => {
    const lean = { key: 'RANGERS', kind: 'game', product_id: 22 };
    publicGet.mockResolvedValue(reply({ ...myProjects(), projects: [lean] }));
    const result = await fetchMyProjects();
    expect(result.ok && result.value.projects[0]).toEqual({
      key: 'RANGERS',
      title: 'RANGERS',
      kind: 'game',
      game: '',
      expansion_code: '',
      visibility: 'private',
      retail: false,
      base: null,
      product_id: 22,
      head: 0,
      owner: null,
      can_push: false,
      can_manage: false,
      storage: null,
    });

    // A kind the client does not know, a project with no key: refused whole.
    publicGet.mockResolvedValue(reply({ ...myProjects(), projects: [{ ...NIGHTOPS_PROJECT, kind: 'app' }] }));
    expect((await fetchMyProjects()).ok).toBe(false);
    publicGet.mockResolvedValue(reply({ ...myProjects(), projects: [{ ...NIGHTOPS_PROJECT, key: '' }] }));
    expect((await fetchMyProjects()).ok).toBe(false);
  });

  it('reads the games and titles leniently: none is none, a malformed one is left out', async () => {
    const { games: _games, titles: _titles, ...rest } = myProjects();
    publicGet.mockResolvedValue(reply(rest));
    const none = await fetchMyProjects();
    expect(none.ok && [none.value.games, none.value.titles]).toEqual([[], []]);

    publicGet.mockResolvedValue(reply({ ...rest, games: [{ key: 'JO', title: 'Joint Operations', game: 'jo' }, { title: 'no key' }, 'JO'], titles: ['jo', 7, ''] }));
    const some = await fetchMyProjects();
    expect(some.ok && [some.value.games, some.value.titles]).toEqual([[{ key: 'JO', title: 'Joint Operations', game: 'jo' }], ['jo']]);
  });

  it('makes a project, changes its name and who gets it, and deletes it, by its key', async () => {
    publicPost.mockResolvedValue(reply({ project: NIGHTOPS_PROJECT }, 201));
    const body = { key: 'NIGHTOPS', display_name: 'Night Ops', kind: 'expansion' as const, base: 'JO', expansion_code: 'nightops', visibility: 'public' as const };
    expect(await createProject(body)).toEqual({ ok: true, value: NIGHTOPS_PROJECT });
    expect(publicPost).toHaveBeenCalledWith('/products', body);

    publicPatch.mockResolvedValue(reply({ project: { ...NIGHTOPS_PROJECT, title: 'Night Ops 2' } }));
    const renamed = await updateProject('NIGHTOPS', { display_name: 'Night Ops 2' });
    expect(renamed.ok && renamed.value.title).toBe('Night Ops 2');
    expect(publicPatch).toHaveBeenCalledWith('/products/NIGHTOPS', { display_name: 'Night Ops 2' });

    publicDel.mockResolvedValue(reply(null, 204));
    expect(await deleteProject('NIGHTOPS')).toEqual({ ok: true, value: true });
    expect(publicDel).toHaveBeenCalledWith('/products/NIGHTOPS');
  });

  it("says each refusal in a player's words, a field's rule for its field, never the server text", async () => {
    const cases: [number, Record<string, unknown>, string, string, string][] = [
      [400, { error: 'invalid_field', field: 'key' }, 'invalid_field', 'key', 'A key is 1 to 16 of A-Z, 0-9 and _.'],
      [400, { error: 'invalid_field', field: 'expansion_code' }, 'invalid_field', 'expansion_code', 'A short name is 1 to 8 lowercase letters and digits.'],
      [400, { error: 'missing_field', field: 'kind' }, 'missing_field', 'kind', 'The server refused one of the values sent. Check them and try again.'],
      [404, { error: 'no_such_base' }, 'no_such_base', '', 'The game that mod is for is not on NovaWorld any more. Pick another.'],
      [409, { error: 'duplicate_key' }, 'duplicate_key', '', 'Another project already has that key. Pick another.'],
      [409, { error: 'duplicate_expansion' }, 'duplicate_expansion', '', 'That game already has a mod with that short name. Pick another.'],
      [403, { error: 'too_many_projects' }, 'too_many_projects', '', 'You have as many projects as an account may have. Delete one to make another.'],
      [429, {}, 'rate_limited', '', 'Too many requests. Try again in a little while.'],
      [401, {}, 'unauthorized', '', 'Your session has ended. Log in again to manage your projects.'],
    ];
    for (const [status, data, code, field, words] of cases) {
      publicPost.mockRejectedValueOnce(refusal(status, { ...data, message: 'raw server text' }));
      const refused = await createProject({ key: 'X', display_name: 'X', kind: 'game', game: 'jo', visibility: 'private' });
      expect(refused).toEqual({ ok: false, error: { code, status, field, message: words } });
    }

    publicDel.mockRejectedValueOnce(refusal(409, { error: 'has_expansion' }));
    const base = await deleteProject('RANGERS');
    expect(!base.ok && base.error.message).toBe('A mod is made for this game. Delete the mod first.');
    publicPatch.mockRejectedValueOnce(refusal(403, { error: 'forbidden' }));
    const notYours = await updateProject('ACIDGAMBIT', { visibility: 'private' });
    expect(!notYours.ok && notYours.error.message).toBe("Only its owner and NovaWorld's admins may change that project.");
    publicDel.mockRejectedValueOnce(refusal(500, { error: 'boom' }));
    const odd = await deleteProject('RANGERS');
    expect(!odd.ok && [odd.error.code, odd.error.message]).toEqual(['unknown', 'Something went wrong. Try again shortly. (HTTP 500)']);
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
