import { flushPromises, mount, type VueWrapper } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import type { Router } from 'vue-router';
import ProjectPage from './ProjectPage.vue';
import { REVISIONS_PAGE, deleteProject, fetchMyProjects, fetchRevisions, updateProject } from '../../api/products';
import { NIGHTOPS_PROJECT, myProjects, project, revision } from '../../test/productFixtures';
import { testRouter } from '../../test/router';

const { refreshSession } = vi.hoisted(() => ({ refreshSession: vi.fn() }));

vi.mock('../../api/products', async (importOriginal) => ({
  REVISIONS_PAGE: (await importOriginal<typeof import('../../api/products')>()).REVISIONS_PAGE,
  deleteProject: vi.fn(),
  fetchMyProjects: vi.fn(),
  fetchRevisions: vi.fn(),
  updateProject: vi.fn(),
}));
vi.mock('../../composables/useSession', async () => {
  const { shallowRef } = await import('vue');
  return { useSession: () => ({ user: shallowRef(null), refresh: refreshSession }) };
});

const mine = vi.mocked(fetchMyProjects);
const history = vi.mocked(fetchRevisions);
const update = vi.mocked(updateProject);
const remove = vi.mocked(deleteProject);

function submitForm(page: VueWrapper, selector: string) {
  return page.get(selector).element.closest('form')!.dispatchEvent(new Event('submit'));
}

describe('ProjectPage', () => {
  let wrapper: VueWrapper | null = null;
  let router: Router;

  async function mountProject(key = 'NIGHTOPS') {
    router = await testRouter(`/me/projects/${key}`, ['/me/projects/:key']);
    wrapper = mount(ProjectPage, { global: { plugins: [router] } });
    await flushPromises();
    return wrapper;
  }

  beforeEach(() => {
    vi.clearAllMocks();
    mine.mockResolvedValue({ ok: true, value: myProjects() });
    history.mockResolvedValue({ ok: true, value: [revision(3, { message: 'night maps' }), revision(2), revision(1)] });
  });

  afterEach(() => {
    wrapper?.unmount();
    wrapper = null;
  });

  it('shows what the project is, who gets it, its size and its history', async () => {
    const page = await mountProject();
    expect(page.get('h1').text()).toBe('Night Ops');
    expect(page.get('#project-kind-line').text()).toMatch(/^NIGHTOPS, A mod of Joint Operations\s+\(short name nightops\)$/);
    expect(page.get('#project-visibility-line').text()).toBe('Public: anyone may get it.');
    expect(page.text()).toContain('r3');
    expect(page.text()).toContain('300 MiB');
    expect(history).toHaveBeenCalledWith('NIGHTOPS', undefined);
    const revisions = page.findAll('li.project-revision');
    expect(revisions).toHaveLength(3);
    expect(revisions[0].text()).toContain('night maps');
    expect(revisions[2].text()).toContain('biggy, 1200 files, 1.1 GiB');
    expect(page.findAll('button').some((b) => b.text() === 'Older revisions')).toBe(false);
    expect(page.text()).not.toContain('—');
  });

  it('reads older revisions a page at a time', async () => {
    const page1 = Array.from({ length: REVISIONS_PAGE }, (_, i) => revision(80 - i));
    history.mockResolvedValueOnce({ ok: true, value: page1 }).mockResolvedValueOnce({ ok: true, value: [revision(30)] });
    const page = await mountProject();
    const older = page.findAll('button').find((b) => b.text() === 'Older revisions')!;
    await older.trigger('click');
    await flushPromises();
    expect(history).toHaveBeenLastCalledWith('NIGHTOPS', 31);
    expect(page.findAll('li.project-revision')).toHaveLength(REVISIONS_PAGE + 1);
  });

  it('says how to push the first revision of an empty one', async () => {
    const page = await mountProject('RANGERS');
    expect(page.get('#project-kind-line').text()).toBe('RANGERS, A game for Delta Force Xtreme 2');
    expect(page.text()).toContain('Nothing pushed yet. In the OpenNova Editor, put your project on NovaWorld as RANGERS, then push to it.');
    expect(history).not.toHaveBeenCalled();
  });

  it('makes it private and renames it, its key staying', async () => {
    update.mockResolvedValueOnce({ ok: true, value: { ...NIGHTOPS_PROJECT, visibility: 'private' } });
    const page = await mountProject();
    await page.get('#project-visibility').trigger('click');
    await flushPromises();
    expect(update).toHaveBeenCalledWith('NIGHTOPS', { visibility: 'private' });
    expect(page.get('[role="status"]').text()).toBe("Only you and NovaWorld's admins may now get Night Ops.");
    expect(page.get('#project-visibility-line').text()).toBe("Private: only you and NovaWorld's admins may get it.");
    expect(page.get('#project-visibility').text()).toBe('Make public');

    await page.get('#project-rename-toggle').trigger('click');
    expect(page.get('#project-rename-hint').text()).toBe('Its key, NIGHTOPS, stays: the editor finds the project by its key.');
    await page.get('#project-rename-name').setValue('  ');
    submitForm(page, '#project-rename-name');
    await flushPromises();
    expect(update).toHaveBeenCalledTimes(1);
    expect(page.get('#project-rename-hint').text()).toBe('Enter a name.');

    update.mockResolvedValueOnce({ ok: true, value: { ...NIGHTOPS_PROJECT, visibility: 'private', title: 'Night Ops Redux' } });
    await page.get('#project-rename-name').setValue(' Night Ops Redux ');
    submitForm(page, '#project-rename-name');
    await flushPromises();
    expect(update).toHaveBeenLastCalledWith('NIGHTOPS', { display_name: 'Night Ops Redux' });
    expect(page.get('h1').text()).toBe('Night Ops Redux');
    expect(page.get('[role="status"]').text()).toBe('Night Ops is now Night Ops Redux.');
    expect(page.find('#project-rename-form').exists()).toBe(false);
  });

  it('deletes it behind its key typed, and says why one stays', async () => {
    remove.mockResolvedValueOnce({ ok: false, error: { code: 'has_expansion', status: 409, field: '', message: 'A mod is made for this game. Delete the mod first.' } });
    const page = await mountProject('RANGERS');
    await page.get('#project-delete-toggle').trigger('click');
    expect(page.get('#project-delete-form').text()).toContain('Delete Rangers? Its history and files go for good, and the editor can no longer pull or push RANGERS.');
    const submit = page.get('#project-delete-form button[type="submit"]');
    expect(submit.attributes('disabled')).toBeDefined();
    await page.get('#project-delete-confirm').setValue('RANGERS');
    expect(submit.attributes('disabled')).toBeUndefined();
    submitForm(page, '#project-delete-confirm');
    await flushPromises();
    expect(remove).toHaveBeenCalledWith('RANGERS');
    expect(page.get('[role="alert"]').text()).toBe('A mod is made for this game. Delete the mod first.');

    remove.mockResolvedValueOnce({ ok: true, value: true });
    submitForm(page, '#project-delete-confirm');
    await flushPromises();
    expect(router.currentRoute.value.path).toBe('/me/projects');
  });

  it('sends an ended session to the login, from a change or a read', async () => {
    update.mockResolvedValueOnce({ ok: false, error: { code: 'unauthorized', status: 401, field: '', message: 'ended' } });
    const page = await mountProject();
    await page.get('#project-visibility').trigger('click');
    await flushPromises();
    expect(refreshSession).toHaveBeenCalled();
    expect(router.currentRoute.value.fullPath).toBe('/login?redirect=/me/projects/NIGHTOPS');
    wrapper?.unmount();

    refreshSession.mockClear();
    mine.mockResolvedValue({ ok: false, error: { code: 'unauthorized', status: 401, field: '', message: 'ended' } });
    await mountProject('RANGERS');
    expect(refreshSession).toHaveBeenCalled();
    expect(router.currentRoute.value.fullPath).toBe('/login?redirect=/me/projects/RANGERS');
  });

  it("offers no change of a project the service says it may not manage, and says when there is none", async () => {
    mine.mockResolvedValue({ ok: true, value: myProjects({ projects: [project('LOCKED', { can_manage: false })] }) });
    const page = await mountProject('LOCKED');
    expect(page.text()).toContain("Only its owner and NovaWorld's admins may change it.");
    expect(page.find('#project-visibility').exists()).toBe(false);
    expect(page.find('#project-delete-toggle').exists()).toBe(false);
    wrapper?.unmount();

    const missing = await mountProject('NOPE');
    expect(missing.get('h1').text()).toBe('No such project');
    expect(missing.text()).toContain('No project of yours has the key NOPE.');
  });
});
