import { AxiosError, AxiosHeaders, type AxiosResponse } from 'axios';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import { adminClient, apiClient } from './client';
import {
  REVISIONS_PAGE,
  createContentProject,
  deleteContentProject,
  fetchContentProjects,
  fetchContentRevisions,
  updateContentProject,
} from './content';

vi.mock('./client', () => ({
  apiClient: { get: vi.fn() },
  adminClient: { post: vi.fn(), patch: vi.fn(), delete: vi.fn() },
}));

const get = vi.mocked(apiClient.get);
const post = vi.mocked(adminClient.post);
const patch = vi.mocked(adminClient.patch);
const remove = vi.mocked(adminClient.delete);

function reply(data: unknown, status = 200) {
  return { status, data } as AxiosResponse;
}

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

const project = {
  key: 'JO',
  title: 'Joint Operations',
  visibility: 'private',
  retail: true,
  base: null,
  data_set_id: 2,
  head: 4,
  can_push: true,
  storage: { files: 1200, bytes: 1_200_000_000 },
};

describe('the content store routes', () => {
  beforeEach(() => vi.clearAllMocks());

  it('reads the projects, an expansion naming its base, and refuses a malformed one', async () => {
    get.mockResolvedValue(reply({ projects: [project, { ...project, key: 'JOX01', base: 'JO', data_set_id: null, storage: undefined }] }));
    const read = await fetchContentProjects();
    expect(read.ok && read.value.map((p) => [p.key, p.base, p.data_set_id, p.storage])).toEqual([
      ['JO', null, 2, { files: 1200, bytes: 1_200_000_000 }],
      ['JOX01', 'JO', null, null],
    ]);
    expect(get).toHaveBeenCalledWith('/content/projects', expect.anything());
    get.mockResolvedValue(reply({ projects: [{ ...project, visibility: 'friends' }] }));
    expect((await fetchContentProjects()).ok).toBe(false);
  });

  it('reads a history a page at a time', async () => {
    const revision = { number: 4, author: 'taylor', message: 'More', manifest_sha256: 'ab', file_count: 3, bytes: 9, created_at: '2026-10-25 12:00:00' };
    get.mockResolvedValue(reply({ revisions: [revision] }));
    const read = await fetchContentRevisions('JO', 5);
    expect(read.ok && read.value[0].message).toBe('More');
    expect(get).toHaveBeenCalledWith('/content/projects/JO/revisions', expect.objectContaining({ params: { limit: REVISIONS_PAGE, before: 5 } }));
    await fetchContentRevisions('JO');
    expect(get).toHaveBeenLastCalledWith('/content/projects/JO/revisions', expect.objectContaining({ params: { limit: REVISIONS_PAGE } }));
  });

  it('makes, changes and deletes a project, mapping the refusals', async () => {
    post.mockResolvedValue(reply({ project }, 201));
    const made = await createContentProject({ key: 'JO', title: 'Joint Operations', visibility: 'private', retail: true, data_set_id: 2, base: null });
    expect(made.ok && made.value.key).toBe('JO');
    expect(post).toHaveBeenCalledWith('/content/projects', expect.objectContaining({ key: 'JO', retail: true }));

    patch.mockResolvedValue(reply({ project: { ...project, visibility: 'public' } }));
    const changed = await updateContentProject('JO', { visibility: 'public' });
    expect(changed.ok && changed.value.visibility).toBe('public');
    expect(patch).toHaveBeenCalledWith('/content/projects/JO', { visibility: 'public' });

    remove.mockResolvedValue(reply('', 204));
    expect(await deleteContentProject('JO')).toEqual({ ok: true, value: true });
    expect(remove).toHaveBeenCalledWith('/content/projects/JO');

    const cases: [number, string, string][] = [
      [409, 'duplicate_key', 'already has that key'],
      [409, 'data_set_taken', 'already update that data set'],
      [400, 'no_such_base', 'No content project has that base key'],
      [409, 'has_expansion', "expansion's content project builds on this one"],
    ];
    for (const [status, code, text] of cases) {
      remove.mockRejectedValueOnce(refusal(status, { error: code }));
      const refused = await deleteContentProject('JO');
      expect(!refused.ok && refused.error.code).toBe(code);
      expect(!refused.ok && refused.error.message).toContain(text);
    }
  });
});
