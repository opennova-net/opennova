import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import ContentSection from './ContentSection.vue';
import {
  buildContentRelease,
  createContentProject,
  deleteContentProject,
  fetchContentProjects,
  fetchContentRevisions,
  updateContentProject,
} from '../../api/content';
import { fetchDataSets } from '../../api/dataSets';
import { testRouter } from '../../test/router';
import type { ContentProject } from '../../types/content';

vi.mock('../../api/content', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../../api/content')>()),
  fetchContentProjects: vi.fn(),
  buildContentRelease: vi.fn(),
  fetchContentRevisions: vi.fn(),
  createContentProject: vi.fn(),
  updateContentProject: vi.fn(),
  deleteContentProject: vi.fn(),
}));
vi.mock('../../api/dataSets', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../../api/dataSets')>()),
  fetchDataSets: vi.fn(),
}));

const read = vi.mocked(fetchContentProjects);
const history = vi.mocked(fetchContentRevisions);
const create = vi.mocked(createContentProject);
const update = vi.mocked(updateContentProject);
const remove = vi.mocked(deleteContentProject);
const dataSets = vi.mocked(fetchDataSets);
const release = vi.mocked(buildContentRelease);

function project(key: string, extra: Partial<ContentProject> = {}): ContentProject {
  return {
    key,
    title: `The ${key}`,
    visibility: 'public',
    retail: false,
    base: null,
    data_set_id: null,
    head: 3,
    can_push: true,
    storage: { files: 10, bytes: 2048 },
    ...extra,
  };
}

describe('ContentSection', () => {
  let wrapper: ReturnType<typeof mount> | null = null;

  async function mountSection() {
    const router = await testRouter('/admin/content');
    wrapper = mount(ContentSection, { global: { plugins: [router] } });
    await flushPromises();
    return wrapper;
  }

  beforeEach(() => {
    vi.clearAllMocks();
    read.mockResolvedValue({ ok: true, value: [project('ACIDGAMBIT'), project('JO', { visibility: 'private', retail: true, data_set_id: 2 })] });
    dataSets.mockResolvedValue({
      ok: true,
      value: { store: { kind: 'local' }, titles: [], games: [], data_sets: [{ id: 2, display_name: 'Joint Operations' }] },
    } as never);
  });

  afterEach(() => {
    wrapper?.unmount();
    wrapper = null;
  });

  it('lists the projects, who reads each, and what each updates', async () => {
    const page = await mountSection();
    expect(page.get('h1').text()).toBe('Content');
    const rows = page.findAll('.content-row');
    expect(rows).toHaveLength(2);
    expect(rows[0].text()).toContain('Anyone');
    expect(rows[1].text()).toContain('Admins only');
    expect(rows[1].text()).toContain('a retail import');
    expect(rows[1].text()).toContain('Joint Operations');
    expect(rows[1].text()).toContain('10 files');
  });

  it('shows a history, and older revisions on asking', async () => {
    const revisions = Array.from({ length: 50 }, (_, i) => ({
      number: 60 - i,
      author: 'taylor',
      message: `Change ${60 - i}`,
      manifest_sha256: 'ab',
      file_count: 1,
      bytes: 1,
      created_at: '2026-10-25 12:00:00',
    }));
    history.mockResolvedValueOnce({ ok: true, value: revisions });
    history.mockResolvedValueOnce({ ok: true, value: [{ ...revisions[0], number: 10, message: 'Change 10' }] });
    const page = await mountSection();
    const button = page.findAll('button').find((b) => b.text().startsWith('History') && b.text().includes('ACIDGAMBIT'));
    await button!.trigger('click');
    await flushPromises();
    expect(history).toHaveBeenCalledWith('ACIDGAMBIT', undefined);
    expect(page.findAll('.content-history li')).toHaveLength(50);
    const older = page.findAll('button').find((b) => b.text() === 'Older revisions');
    await older!.trigger('click');
    await flushPromises();
    expect(history).toHaveBeenLastCalledWith('ACIDGAMBIT', 11);
    expect(page.findAll('.content-history li')).toHaveLength(51);
  });

  it('makes a project, its key in capitals', async () => {
    create.mockResolvedValue({ ok: true, value: project('NIFTY') });
    const page = await mountSection();
    await page.get('#content-new-key').setValue('nifty');
    await page.get('#content-new-title').setValue('Our expansion');
    await page.get('#content-new-base').setValue('ACIDGAMBIT');
    await page.get('form.grid').trigger('submit');
    await flushPromises();
    expect(create).toHaveBeenCalledWith(expect.objectContaining({ key: 'NIFTY', title: 'Our expansion', base: 'ACIDGAMBIT', visibility: 'private' }));
    expect(page.get('[role="status"]').text()).toContain('NIFTY is made');
  });

  it('makes a project public, and deletes one only once its key is typed', async () => {
    update.mockResolvedValue({ ok: true, value: project('JO') });
    remove.mockResolvedValue({ ok: true, value: true });
    const page = await mountSection();
    const make = page.findAll('button').find((b) => b.text().startsWith('Make public'));
    await make!.trigger('click');
    await flushPromises();
    expect(update).toHaveBeenCalledWith('JO', { visibility: 'public' });

    const begin = page.findAll('button').find((b) => b.text().startsWith('Delete') && b.text().includes('JO') && !b.text().includes('ACIDGAMBIT'));
    await begin!.trigger('click');
    const confirm = () => page.findAll('button').find((b) => b.text() === 'Delete now')!;
    expect(confirm().attributes('disabled')).toBeDefined();
    await page.get('#content-delete-JO').setValue('JO');
    expect(confirm().attributes('disabled')).toBeUndefined();
    await page.findAll('form').find((f) => f.find('#content-delete-JO').exists())!.trigger('submit');
    await flushPromises();
    expect(remove).toHaveBeenCalledWith('JO');
  });

  it('builds a release of a revision, its version checked first', async () => {
    release.mockResolvedValue({ ok: true, value: { id: 9, name: 'JO 1.0.1' } });
    const page = await mountSection();
    const begin = page.findAll('button').find((b) => b.text().startsWith('Release') && b.text().includes('JO') && !b.text().includes('ACIDGAMBIT'));
    await begin!.trigger('click');
    const form = () => page.findAll('form').find((f) => f.find('#content-release-version-JO').exists())!;
    await page.get('#content-release-version-JO').setValue('1.x');
    await form().trigger('submit');
    await flushPromises();
    expect(release).not.toHaveBeenCalled();
    expect(form().text()).toContain('one to four numbers');
    await page.get('#content-release-version-JO').setValue('1.0.1');
    await form().trigger('submit');
    await flushPromises();
    expect(release).toHaveBeenCalledWith('JO', { version: '1.0.1', revision: 3 });
    expect(page.get('[role="status"]').text()).toContain('JO 1.0.1 is building');
  });

  it('says why a read failed', async () => {
    read.mockResolvedValue({ ok: false, error: { code: 'network', status: null, message: 'Could not reach the server.' } });
    const page = await mountSection();
    expect(page.get('[role="alert"]').text()).toContain('Could not reach the server.');
  });
});
