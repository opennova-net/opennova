import * as actionReview from '../../composables/useConfirm';
import { flushPromises, mount, type VueWrapper } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi, type MockInstance } from 'vitest';
import type { Router } from 'vue-router';
import ProjectPage from './ProjectPage.vue';
import {
  REVISIONS_PAGE,
  buildRelease,
  deleteProject,
  deleteProjectRelease,
  fetchMyProjects,
  fetchProjectReleases,
  fetchRevisions,
  publishProjectRelease,
  retireProjectRelease,
  updateProject,
} from '../../api/products';
import { NIGHTOPS_PROJECT, myProjects, nightOpsRelease, project, revision } from '../../test/productFixtures';
import { testRouter } from '../../test/router';

const { refreshSession } = vi.hoisted(() => ({ refreshSession: vi.fn() }));

vi.mock('../../api/products', async (importOriginal) => ({
  REVISIONS_PAGE: (await importOriginal<typeof import('../../api/products')>()).REVISIONS_PAGE,
  buildRelease: vi.fn(),
  deleteProject: vi.fn(),
  deleteProjectRelease: vi.fn(),
  fetchMyProjects: vi.fn(),
  fetchProjectReleases: vi.fn(),
  fetchRevisions: vi.fn(),
  publishProjectRelease: vi.fn(),
  retireProjectRelease: vi.fn(),
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
const releases = vi.mocked(fetchProjectReleases);
const build = vi.mocked(buildRelease);
const publish = vi.mocked(publishProjectRelease);
const retire = vi.mocked(retireProjectRelease);
const removeRelease = vi.mocked(deleteProjectRelease);

function submitForm(page: VueWrapper, selector: string) {
  return page.get(selector).element.closest('form')!.dispatchEvent(new Event('submit'));
}

function buttonNamed(scope: Pick<VueWrapper, 'findAll'>, text: string) {
  return scope.findAll('button').find((b) => b.text().startsWith(text))!;
}

describe('ProjectPage', () => {
  let wrapper: VueWrapper | null = null;
  let router: Router;
  let confirm: MockInstance<typeof actionReview.confirmAction>;

  async function mountProject(key = 'NIGHTOPS') {
    router = await testRouter(`/me/projects/${key}`, ['/me/projects/:key']);
    wrapper = mount(ProjectPage, { global: { plugins: [router] } });
    await flushPromises();
    return wrapper;
  }

  beforeEach(() => {
    vi.clearAllMocks();
    confirm = vi.spyOn(actionReview, 'confirmAction').mockResolvedValue(true);
    mine.mockResolvedValue({ ok: true, value: myProjects() });
    history.mockResolvedValue({ ok: true, value: [revision(3, { message: 'night maps' }), revision(2), revision(1)] });
    releases.mockResolvedValue({ ok: true, value: [] });
  });

  afterEach(() => {
    wrapper?.unmount();
    wrapper = null;
    confirm.mockRestore();
    vi.useRealTimers();
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

  it('lists its releases with their status, error and build log, and what the account stores', async () => {
    releases.mockResolvedValue({
      ok: true,
      value: [
        nightOpsRelease(4, { status: 'building', bytes: 0, sha256: '', sections: [], build_log: '$ opennova-project export\n' }),
        nightOpsRelease(3, { status: 'invalid', error: 'The package names no NIGHTOPS.CMD.' }),
        nightOpsRelease(2, { status: 'published' }),
        nightOpsRelease(1, { status: 'retired', created_by: 'admin root' }),
      ],
    });
    const page = await mountProject();
    expect(releases).toHaveBeenCalledWith('NIGHTOPS', expect.anything());
    expect(page.get('#project-published-line').text()).toBe('Players update to 0.2.0.');
    expect(page.get('#project-releases').element.closest('section')!.textContent).toContain('Players install a published public mod from the NovaWorld panel in game.');

    const rows = page.findAll('li.project-release');
    expect(rows.map((r) => r.get('span.font-mono').text())).toEqual(['0.4.0', '0.3.0', '0.2.0', '0.1.0']);
    expect(rows[0].text()).toContain('Building');
    expect(rows[0].text()).toMatch(/from r3 on its game's r2,\s+made .* by biggy/);
    expect(rows[0].get('details').attributes('open')).toBeDefined();
    expect(rows[0].get('pre').text()).toBe('$ opennova-project export');
    expect(rows[0].findAll('button')).toHaveLength(0);
    expect(rows[1].text()).toContain('Invalid');
    expect(rows[1].get('p.text-danger').text()).toBe('The package names no NIGHTOPS.CMD.');
    expect(rows[1].findAll('button').map((b) => b.text())).toEqual(['Delete release 0.3.0']);
    expect(rows[2].text()).toContain('Published');
    expect(rows[2].text()).toContain('106 MiB');
    expect(rows[2].text()).toContain("Servers' data ready");
    expect(rows[2].findAll('button').map((b) => b.text())).toEqual(['Retire release 0.2.0']);
    expect(rows[3].text()).toContain('by admin root');
    expect(rows[3].findAll('button').map((b) => b.text())).toEqual(['Delete release 0.1.0']);
    expect(page.get('#project-release-usage').text()).toBe(
      'Your account stores 512 MiB of the 4 GiB it may; 212 MiB of it is release packages. Deleting a release frees its package.',
    );
    expect(page.text()).not.toContain('—');
  });

  it('reads the releases again every few seconds while one builds, and stops once none does', async () => {
    vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout'] });
    releases.mockResolvedValue({ ok: true, value: [nightOpsRelease(4, { status: 'building', sections: [] })] });
    await mountProject();
    expect(releases).toHaveBeenCalledTimes(1);
    releases.mockResolvedValue({ ok: true, value: [nightOpsRelease(4)] });
    await vi.advanceTimersByTimeAsync(3_000);
    await flushPromises();
    expect(releases).toHaveBeenCalledTimes(2);
    expect(wrapper!.get('li.project-release').text()).toContain('Draft');
    await vi.advanceTimersByTimeAsync(60_000);
    expect(releases).toHaveBeenCalledTimes(2);
  });

  it('builds a release from the newest revision once its version reads right, and says why one is refused', async () => {
    const page = await mountProject();
    await page.get('#project-build-toggle').trigger('click');
    expect((page.get('#project-build-revision').element as HTMLInputElement).value).toBe('3');
    expect(page.get('#project-build-form').text()).toContain("NovaWorld packs the revision whole, on its game's newest revision");
    expect(page.get('#project-build-hint').text()).toBe('A version is one to four numbers between dots, such as 1.0 or 0.2.1. r3 is the newest revision.');
    await page.get('#project-build-version').setValue('0.5.x');
    submitForm(page, '#project-build-version');
    await flushPromises();
    expect(build).not.toHaveBeenCalled();
    expect(page.get('#project-build-hint').text()).toBe('A version is one to four numbers between dots, such as 1.0 or 0.2.1.');

    build.mockResolvedValueOnce({
      ok: false,
      error: { code: 'release_in_progress', status: 409, field: '', message: 'One of your releases is still building or being checked. Start the next once it is done.' },
    });
    await page.get('#project-build-version').setValue('0.5.0');
    submitForm(page, '#project-build-version');
    await flushPromises();
    expect(build).toHaveBeenCalledWith('NIGHTOPS', { version: '0.5.0', revision: 3 });
    expect(page.get('[role="alert"]').text()).toBe('One of your releases is still building or being checked. Start the next once it is done.');
    expect(page.find('#project-build-form').exists()).toBe(true);

    build.mockResolvedValueOnce({ ok: true, value: nightOpsRelease(5, { status: 'building', version: '0.5.0' }) });
    releases.mockResolvedValue({ ok: true, value: [nightOpsRelease(5, { status: 'building', version: '0.5.0' })] });
    await page.get('#project-build-revision').setValue(2);
    submitForm(page, '#project-build-version');
    await flushPromises();
    expect(build).toHaveBeenLastCalledWith('NIGHTOPS', { version: '0.5.0', revision: 2 });
    expect(page.get('[role="status"]').text()).toBe('0.5.0 is building. It shows here as a draft to publish once NovaWorld has checked it.');
    expect(page.find('#project-build-form').exists()).toBe(false);
    expect(page.findAll('li.project-release')).toHaveLength(1);
  });

  it("publishes a private project's draft and makes the project public, unless that is unchecked", async () => {
    const privateOps = { ...NIGHTOPS_PROJECT, visibility: 'private' as const, release: null };
    mine.mockResolvedValue({ ok: true, value: myProjects({ projects: [privateOps] }) });
    releases.mockResolvedValue({ ok: true, value: [nightOpsRelease(3)] });
    update.mockResolvedValueOnce({ ok: true, value: { ...privateOps, visibility: 'public' } });
    publish.mockResolvedValueOnce({ ok: true, value: nightOpsRelease(3, { status: 'published' }) });
    const page = await mountProject();
    expect(page.get('#project-published-line').text()).toBe('Nothing published yet.');
    await buttonNamed(page.get('li.project-release'), 'Publish').trigger('click');
    const form = page.get('#project-publish-form');
    expect(form.text()).toContain('Publish 0.3.0 of Night Ops?');
    expect(form.text()).toMatch(/Make Night Ops public too, so other players find it\s+in game and can host it\./);
    expect(form.text()).toContain('Private means unlisted, not secret');
    expect((page.get('#project-publish-public').element as HTMLInputElement).checked).toBe(true);
    submitForm(page, '#project-publish-confirm');
    await flushPromises();
    expect(update).toHaveBeenCalledWith('NIGHTOPS', { visibility: 'public' });
    expect(publish).toHaveBeenCalledWith('NIGHTOPS', 3);
    expect(update.mock.invocationCallOrder[0]).toBeLessThan(publish.mock.invocationCallOrder[0]);
    expect(page.get('[role="status"]').text()).toBe('0.3.0 is published, and anyone may now get Night Ops.');
    expect(page.find('#project-publish-form').exists()).toBe(false);

    // Unchecked, it stays private.
    wrapper?.unmount();
    vi.clearAllMocks();
    mine.mockResolvedValue({ ok: true, value: myProjects({ projects: [privateOps] }) });
    releases.mockResolvedValue({ ok: true, value: [nightOpsRelease(3)] });
    publish.mockResolvedValueOnce({ ok: true, value: nightOpsRelease(3, { status: 'published' }) });
    const again = await mountProject();
    await buttonNamed(again.get('li.project-release'), 'Publish').trigger('click');
    await again.get('#project-publish-public').setValue(false);
    submitForm(again, '#project-publish-confirm');
    await flushPromises();
    expect(update).not.toHaveBeenCalled();
    expect(publish).toHaveBeenCalledWith('NIGHTOPS', 3);
    expect(again.get('[role="status"]').text()).toBe('0.3.0 is published.');
  });

  it("asks nothing about who gets a public project's release, and stops a publish whose visibility change is refused", async () => {
    releases.mockResolvedValue({ ok: true, value: [nightOpsRelease(3)] });
    const page = await mountProject();
    await buttonNamed(page.get('li.project-release'), 'Publish').trigger('click');
    expect(page.find('#project-publish-form').exists()).toBe(true);
    expect(page.find('#project-publish-public').exists()).toBe(false);
    wrapper?.unmount();

    mine.mockResolvedValue({ ok: true, value: myProjects({ projects: [{ ...NIGHTOPS_PROJECT, visibility: 'private' }] }) });
    update.mockResolvedValueOnce({ ok: false, error: { code: 'forbidden', status: 403, field: '', message: "Only its owner and NovaWorld's admins may change that project." } });
    const refusedPage = await mountProject();
    await buttonNamed(refusedPage.get('li.project-release'), 'Publish').trigger('click');
    submitForm(refusedPage, '#project-publish-confirm');
    await flushPromises();
    expect(publish).not.toHaveBeenCalled();
    expect(refusedPage.get('[role="alert"]').text()).toBe("Only its owner and NovaWorld's admins may change that project.");
  });

  it('retires a published release and deletes a retired one behind a confirmation, and says why one stays', async () => {
    releases.mockResolvedValue({ ok: true, value: [nightOpsRelease(2, { status: 'published' }), nightOpsRelease(1, { status: 'retired' })] });
    retire.mockResolvedValueOnce({ ok: true, value: nightOpsRelease(2, { status: 'retired' }) });
    const page = await mountProject();
    const rows = () => page.findAll('li.project-release');
    await buttonNamed(rows()[0], 'Retire').trigger('click');
    await flushPromises();
    expect(confirm).toHaveBeenLastCalledWith('Retire 0.2.0? The update service stops serving it; servers keep what they run.');
    expect(retire).toHaveBeenCalledWith('NIGHTOPS', 2);
    expect(page.get('[role="status"]').text()).toBe('0.2.0 is retired.');

    confirm.mockResolvedValueOnce(false);
    await buttonNamed(rows()[1], 'Delete').trigger('click');
    await flushPromises();
    expect(removeRelease).not.toHaveBeenCalled();

    removeRelease.mockResolvedValueOnce({ ok: false, error: { code: 'published', status: 409, field: '', message: 'A published release cannot be deleted. Retire it first.' } });
    await buttonNamed(rows()[1], 'Delete').trigger('click');
    await flushPromises();
    expect(removeRelease).toHaveBeenCalledWith('NIGHTOPS', 1);
    expect(page.get('[role="alert"]').text()).toBe('A published release cannot be deleted. Retire it first.');

    removeRelease.mockResolvedValueOnce({ ok: true, value: true });
    await buttonNamed(rows()[1], 'Delete').trigger('click');
    await flushPromises();
    expect(confirm).toHaveBeenLastCalledWith('Delete 0.1.0? Its package goes for good, and stops counting against what your account may store.');
    expect(page.get('[role="status"]').text()).toBe('0.1.0 is deleted.');
  });

  it('says a release gone is gone, and a quota refusal in its words', async () => {
    releases.mockResolvedValue({ ok: true, value: [nightOpsRelease(3)] });
    const page = await mountProject();
    publish.mockResolvedValueOnce({ ok: false, error: { code: 'not_found', status: 404, field: '', message: 'That project is not on NovaWorld, or it is private.' } });
    await buttonNamed(page.get('li.project-release'), 'Publish').trigger('click');
    submitForm(page, '#project-publish-confirm');
    await flushPromises();
    expect(page.get('[role="alert"]').text()).toBe('That release is no longer on NovaWorld.');
    expect(releases.mock.calls.length).toBeGreaterThan(1);

    const words = 'That would take your account past the 4 GiB it may store (3.9 GiB in use). Delete a release or a project you no longer need to make room.';
    build.mockResolvedValueOnce({ ok: false, error: { code: 'quota_exceeded', status: 413, field: '', message: words } });
    await page.get('#project-build-toggle').trigger('click');
    await page.get('#project-build-version').setValue('1.0');
    submitForm(page, '#project-build-version');
    await flushPromises();
    expect(page.get('[role="alert"]').text()).toBe(words);
  });

  it('offers no release of a project it may not release, and none before a push', async () => {
    mine.mockResolvedValue({ ok: true, value: myProjects({ projects: [project('LOCKED', { can_manage: false, can_release: false, head: 2 }), project('RANGERS')] }) });
    const locked = await mountProject('LOCKED');
    expect(releases).not.toHaveBeenCalled();
    expect(locked.text()).toContain("Only its owner and NovaWorld's admins may release it.");
    expect(locked.find('#project-build-toggle').exists()).toBe(false);
    wrapper?.unmount();

    const empty = await mountProject('RANGERS');
    expect(releases).toHaveBeenCalledWith('RANGERS', expect.anything());
    expect(empty.text()).toContain('No release yet.');
    expect(empty.text()).toContain('Push a revision from the editor to build a release.');
    expect(empty.text()).toContain('A published game lists its servers in a lobby of its own and, while public, has a download on the Downloads page.');
    expect(empty.find('#project-build-toggle').exists()).toBe(false);
  });
});
