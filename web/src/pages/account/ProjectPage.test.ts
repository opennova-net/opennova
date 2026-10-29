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

// The signed-in account's role: a player's unless a test makes it an admin's.
const { refreshSession, session } = vi.hoisted(() => ({ refreshSession: vi.fn(), session: { role: 'player' } }));

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
  return { useSession: () => ({ user: shallowRef({ username: 'biggy', role: session.role }), refresh: refreshSession }) };
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

  async function mountProject(key = 'NIGHTOPS', query = '') {
    router = await testRouter(`/me/projects/${key}${query}`, ['/me/projects/:key']);
    wrapper = mount(ProjectPage, { global: { plugins: [router] } });
    await flushPromises();
    return wrapper;
  }

  beforeEach(() => {
    vi.clearAllMocks();
    session.role = 'player';
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

  it('changes a game lobby before locking, preserving a current lobby outside the available list', async () => {
    const lobby = { slug: 'on_42_consumer', display_name: 'Private community', protocol: { code: 'jo', name: 'JO', version: '16' }, gate_tag_file: 'YWJj' };
    const game = project('MYGAME', { title: 'My game', lobby });
    mine.mockResolvedValue({ ok: true, value: myProjects({ projects: [game] }) });
    const page = await mountProject('MYGAME');
    expect(page.get('#project-lobby-current').text()).toBe('Private community');
    await page.get('#project-lobby-edit').trigger('click');
    expect(page.findAll('#project-lobby option').map((option) => option.attributes('value'))).toEqual(['', 'on_42_consumer', 'acidgambit_0_consumer']);
    await page.get('#project-lobby').setValue('acidgambit_0_consumer');
    update.mockResolvedValue({ ok: true, value: { ...game, lobby: { ...lobby, slug: 'acidgambit_0_consumer', display_name: 'OpenNova' } } });
    submitForm(page, '#project-lobby');
    await flushPromises();
    expect(update).toHaveBeenCalledWith('MYGAME', { lobby: 'acidgambit_0_consumer' });
    expect(page.get('#project-lobby-current').text()).toBe('OpenNova');
    expect(page.find('#project-lobby-form').exists()).toBe(false);

    await page.get('#project-lobby-edit').trigger('click');
    await page.get('input[name="project-lobby-mode"][value="new"]').setValue(true);
    await page.get('#project-lobby-name').setValue('New community');
    update.mockResolvedValue({ ok: true, value: { ...game, lobby: { ...lobby, display_name: 'New community' } } });
    submitForm(page, '#project-lobby-form');
    await flushPromises();
    expect(update).toHaveBeenLastCalledWith('MYGAME', { new_lobby_name: 'New community' });
  });

  it('shows inherited and locked lobbies without offering changes', async () => {
    const lobby = { slug: 'acidgambit_0_consumer', display_name: 'Acid Gambit', protocol: { code: 'jo', name: 'JO', version: '16' }, gate_tag_file: 'YWJj' };
    mine.mockResolvedValue({ ok: true, value: myProjects({ projects: [{ ...NIGHTOPS_PROJECT, lobby }] }) });
    const page = await mountProject();
    expect(page.get('#project-lobby-settings').text()).toContain('Uses its base game’s lobby.');
    expect(page.find('#project-lobby-edit').exists()).toBe(false);
    wrapper?.unmount();
    mine.mockResolvedValue({ ok: true, value: myProjects({ projects: [project('MYGAME', { lobby, lobby_locked: true })] }) });
    const locked = await mountProject('MYGAME');
    expect(locked.get('#project-lobby-locked').text()).toContain('release build has started');
    expect(locked.find('#project-lobby-edit').exists()).toBe(false);
  });

  it('refreshes the lock if a release starts while editing the lobby', async () => {
    const game = project('MYGAME');
    mine.mockResolvedValue({ ok: true, value: myProjects({ projects: [game] }) });
    const page = await mountProject('MYGAME');
    await page.get('#project-lobby-edit').trigger('click');
    update.mockResolvedValue({ ok: false, error: { code: 'lobby_locked', status: 409, field: '', message: 'The lobby is locked.' } });
    mine.mockResolvedValue({ ok: true, value: myProjects({ projects: [{ ...game, lobby_locked: true }] }) });
    submitForm(page, '#project-lobby-form');
    await flushPromises();
    expect(page.get('[role="alert"]').text()).toBe('The lobby is locked.');
    expect(page.find('#project-lobby-form').exists()).toBe(false);
    expect(page.find('#project-lobby-edit').exists()).toBe(false);
  });

  it('heads the page with the project, its facts in one line, its actions and its two tabs, and lists its history on the second', async () => {
    const page = await mountProject();
    expect(page.get('h1').text()).toBe('Night Ops');
    const crumb = page.get('nav[aria-label="Breadcrumb"] a');
    expect([crumb.text(), crumb.attributes('href')]).toEqual(['Projects', '/me/projects']);
    const meta = page.get('.page-meta');
    expect(meta.findAll(':scope > *').map((e) => e.text())).toEqual(['NIGHTOPS', 'A mod of Joint Operations', 'JO protocol 16', 'nightops', 'Public', 'r3', '300 MiB', 'Players get 0.2.0 (ONE 0.0.22)']);
    expect(page.get('#project-visibility-badge').attributes('title')).toBe('Public: anyone may get it.');
    expect(page.findAll('.page-header button').map((b) => b.text())).toEqual(['Make private', 'Rename', 'Delete']);

    // Releases first; History reads only once its tab shows.
    const tabs = page.findAll('.tab-nav a');
    expect(tabs.map((a) => [a.text(), a.attributes('href'), a.attributes('aria-current')])).toEqual([
      ['Releases', '/me/projects/NIGHTOPS', 'page'],
      ['History', '/me/projects/NIGHTOPS?tab=history', undefined],
    ]);
    expect(page.find('#project-releases').exists()).toBe(true);
    expect(page.find('#project-history').exists()).toBe(false);
    expect(history).not.toHaveBeenCalled();

    await router.push('/me/projects/NIGHTOPS?tab=history');
    await flushPromises();
    expect(page.get('.tab-nav a[aria-current="page"]').text()).toBe('History');
    expect(page.find('#project-releases').exists()).toBe(false);
    expect(history).toHaveBeenCalledWith('NIGHTOPS', undefined);
    const historyTable = page.get('#project-history').element.closest('section')!;
    expect([...historyTable.querySelectorAll('thead th')].map((th) => th.textContent)).toEqual(['Revision', 'Message', 'By', 'Files', 'Size', 'When']);
    const revisions = page.findAll('tr.project-revision');
    expect(revisions).toHaveLength(3);
    expect(revisions[0].text()).toContain('night maps');
    expect(revisions[2].findAll('td').map((td) => td.text()).slice(1, 4)).toEqual(['biggy', '1200', '1.1 GiB']);
    expect(page.findAll('button').some((b) => b.text() === 'Older revisions')).toBe(false);
    expect(page.text()).not.toContain('—');
  });

  it('says a first read failed, and a later one in one line while it keeps what it showed', async () => {
    mine.mockResolvedValueOnce({ ok: false, error: { code: 'network', status: null, field: '', message: 'Could not reach the server.' } });
    const failed = await mountProject();
    expect(failed.text()).toContain('Could not read the project; retrying.');
    wrapper?.unmount();

    const page = await mountProject();
    expect(page.text()).not.toContain('Last refresh failed');
    mine.mockResolvedValue({ ok: false, error: { code: 'network', status: null, field: '', message: 'Could not reach the server.' } });
    build.mockResolvedValueOnce({ ok: true, value: nightOpsRelease(5, { status: 'building', version: '0.5.0' }) });
    await page.get('#project-build-toggle').trigger('click');
    await page.get('#project-build-version').setValue('0.5.0');
    submitForm(page, '#project-build-version');
    await flushPromises();
    expect(page.get('h1').text()).toBe('Night Ops');
    expect(page.text()).toContain('Last refresh failed; retrying.');
  });

  it('reads older revisions a page at a time', async () => {
    const page1 = Array.from({ length: REVISIONS_PAGE }, (_, i) => revision(80 - i));
    history.mockResolvedValueOnce({ ok: true, value: page1 }).mockResolvedValueOnce({ ok: true, value: [revision(30)] });
    const page = await mountProject('NIGHTOPS', '?tab=history');
    const older = page.findAll('button').find((b) => b.text() === 'Older revisions')!;
    await older.trigger('click');
    await flushPromises();
    expect(history).toHaveBeenLastCalledWith('NIGHTOPS', 31);
    expect(page.findAll('tr.project-revision')).toHaveLength(REVISIONS_PAGE + 1);
  });

  it('says an empty one has nothing pushed yet, and reads no history', async () => {
    const page = await mountProject('RANGERS', '?tab=history');
    expect(page.get('#project-kind-line').text()).toBe('Runs as Delta Force Xtreme 2');
    expect(page.get('#project-head').text()).toBe('Nothing pushed yet');
    expect(page.find('#project-published-line').exists()).toBe(false);
    const historyRows = page.get('#project-history').element.closest('section')!.querySelectorAll('tbody tr');
    expect([...historyRows].map((tr) => tr.textContent!.trim())).toEqual(['Nothing pushed yet.']);
    expect(history).not.toHaveBeenCalled();
  });

  it('makes it private and renames it, its key staying', async () => {
    update.mockResolvedValueOnce({ ok: true, value: { ...NIGHTOPS_PROJECT, visibility: 'private' } });
    const page = await mountProject();
    await page.get('#project-visibility').trigger('click');
    await flushPromises();
    expect(update).toHaveBeenCalledWith('NIGHTOPS', { visibility: 'private' });
    expect(page.get('[role="status"]').text()).toBe('Night Ops is now private.');
    expect(page.get('#project-visibility-badge').text()).toBe('Private');
    expect(page.get('#project-visibility').text()).toBe('Make public');

    await page.get('#project-rename-toggle').trigger('click');
    expect(page.get('#project-rename-hint').text()).toBe('');
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
    expect(page.get('#project-delete-form label').text()).toBe('Type RANGERS to delete Rangers and its history for good.');
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
    expect(page.findAll('.page-header button')).toHaveLength(0);
    expect(page.find('#project-visibility').exists()).toBe(false);
    expect(page.find('#project-delete-toggle').exists()).toBe(false);
    wrapper?.unmount();

    const missing = await mountProject('NOPE');
    expect(missing.get('h1').text()).toBe('No such project');
    expect(missing.text()).toContain('No project of yours has the key NOPE.');
  });

  it('lists its releases in a table with their status, error and build log, beside what the account stores', async () => {
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
    expect(page.get('#project-published-line').text()).toBe('Players get 0.2.0 (ONE 0.0.22)');
    const section = page.get('#project-releases').element.closest('section')!;
    // No intro: the table and its buttons speak.
    expect(section.querySelectorAll(':scope > p')).toHaveLength(0);
    expect([...section.querySelectorAll('thead th')].map((th) => th.textContent!.trim())).toEqual(['Game version', 'Status', 'From', 'Made', 'Size', 'Actions']);

    const rows = page.findAll('tr.project-release');
    expect(rows.map((r) => r.get('.release-version').text())).toEqual(['0.4.0', '0.3.0', '0.2.0', '0.1.0']);
    // Each built by the ONE its release names.
    expect(rows.map((r) => r.get('.release-one').text())).toEqual(['ONE 0.0.22', 'ONE 0.0.22', 'ONE 0.0.22', 'ONE 0.0.22']);
    expect(rows[0].text()).toContain('Building');
    expect(rows[0].findAll('td')[1].text()).toBe('r3 on r2');
    expect(rows[0].findAll('td')[2].text()).toMatch(/ by biggy$/);
    expect(rows[0].get('details').attributes('open')).toBeDefined();
    expect(rows[0].get('pre').text()).toBe('$ opennova-project export');
    expect(rows[0].findAll('button')).toHaveLength(0);
    expect(rows[1].text()).toContain('Invalid');
    expect(rows[1].get('.release-error').text()).toBe('The package names no NIGHTOPS.CMD.');
    expect(rows[1].findAll('button').map((b) => b.text())).toEqual(['Delete release 0.3.0']);
    expect(rows[2].text()).toContain('Published');
    expect(rows[2].text()).toContain('106 MiB');
    expect(rows[2].text()).toContain("Servers' data ready");
    expect(rows[2].findAll('button').map((b) => b.text())).toEqual(['Retire release 0.2.0']);
    expect(rows[3].text()).toContain('by admin root');
    expect(rows[3].findAll('button').map((b) => b.text())).toEqual(['Delete release 0.1.0']);
    expect(page.get('#project-release-usage').text()).toBe('512 MiB of 10 GiB stored');
    expect(page.get('#project-release-usage').attributes('title')).toBe('212 MiB of it is release packages');
    expect(page.get('#project-build-toggle').attributes('title')).toBeUndefined();
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
    expect(wrapper!.get('tr.project-release').text()).toContain('Draft');
    await vi.advanceTimersByTimeAsync(60_000);
    expect(releases).toHaveBeenCalledTimes(2);
  });

  it('builds a release from the newest revision once its version reads right, and says why one is refused', async () => {
    const page = await mountProject();
    await page.get('#project-build-toggle').trigger('click');
    expect((page.get('#project-build-revision').element as HTMLInputElement).value).toBe('3');
    expect(page.find('#project-build-toggle').exists()).toBe(false);
    expect(page.get('#project-build-hint').text()).toBe('');
    await page.get('#project-build-version').setValue('0.5.x');
    submitForm(page, '#project-build-version');
    await flushPromises();
    expect(build).not.toHaveBeenCalled();
    expect(page.get('#project-build-hint').text()).toBe('A game version is one to four numbers between dots, such as 0.2.0 or 1.0.');

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
    expect(page.get('[role="status"]').text()).toBe('0.5.0 is building.');
    expect(page.find('#project-build-form').exists()).toBe(false);
    expect(page.findAll('tr.project-release')).toHaveLength(1);
  });

  it("submits a private project's draft for approval and makes the project public, unless that is unchecked", async () => {
    const privateOps = { ...NIGHTOPS_PROJECT, visibility: 'private' as const, release: null };
    mine.mockResolvedValue({ ok: true, value: myProjects({ projects: [privateOps] }) });
    releases.mockResolvedValue({ ok: true, value: [nightOpsRelease(3)] });
    update.mockResolvedValueOnce({ ok: true, value: { ...privateOps, visibility: 'public' } });
    publish.mockResolvedValueOnce({ ok: true, value: nightOpsRelease(3, { status: 'awaiting_approval' }) });
    const page = await mountProject();
    expect(page.find('#project-published-line').exists()).toBe(false);
    await buttonNamed(page.get('tr.project-release'), 'Submit for approval').trigger('click');
    const form = page.get('#project-publish-form');
    expect(form.get('p').text()).toBe("Submit 0.3.0 for an admin's approval?");
    expect(page.get('#project-publish-confirm').text()).toBe('Submit for approval');
    expect(form.get('label').text()).toBe('Make Night Ops public too');
    expect(form.get('label').attributes('title')).toBe('Private means unlisted, not secret.');
    expect((page.get('#project-publish-public').element as HTMLInputElement).checked).toBe(true);
    submitForm(page, '#project-publish-confirm');
    await flushPromises();
    expect(update).toHaveBeenCalledWith('NIGHTOPS', { visibility: 'public' });
    expect(publish).toHaveBeenCalledWith('NIGHTOPS', 3);
    expect(update.mock.invocationCallOrder[0]).toBeLessThan(publish.mock.invocationCallOrder[0]);
    expect(page.get('[role="status"]').text()).toBe("0.3.0 awaits an admin's approval, and Night Ops is now public.");
    expect(page.find('#project-publish-form').exists()).toBe(false);

    // Unchecked, it stays private.
    wrapper?.unmount();
    vi.clearAllMocks();
    mine.mockResolvedValue({ ok: true, value: myProjects({ projects: [privateOps] }) });
    releases.mockResolvedValue({ ok: true, value: [nightOpsRelease(3)] });
    publish.mockResolvedValueOnce({ ok: true, value: nightOpsRelease(3, { status: 'awaiting_approval' }) });
    const again = await mountProject();
    await buttonNamed(again.get('tr.project-release'), 'Submit for approval').trigger('click');
    await again.get('#project-publish-public').setValue(false);
    submitForm(again, '#project-publish-confirm');
    await flushPromises();
    expect(update).not.toHaveBeenCalled();
    expect(publish).toHaveBeenCalledWith('NIGHTOPS', 3);
    expect(again.get('[role="status"]').text()).toBe("0.3.0 awaits an admin's approval.");
  });

  it("asks nothing about who gets a public project's release, and stops a publish whose visibility change is refused", async () => {
    releases.mockResolvedValue({ ok: true, value: [nightOpsRelease(3)] });
    const page = await mountProject();
    await buttonNamed(page.get('tr.project-release'), 'Submit for approval').trigger('click');
    expect(page.find('#project-publish-form').exists()).toBe(true);
    expect(page.find('#project-publish-public').exists()).toBe(false);
    wrapper?.unmount();

    mine.mockResolvedValue({ ok: true, value: myProjects({ projects: [{ ...NIGHTOPS_PROJECT, visibility: 'private' }] }) });
    update.mockResolvedValueOnce({ ok: false, error: { code: 'forbidden', status: 403, field: '', message: "Only its owner and NovaWorld's admins may change that project." } });
    const refusedPage = await mountProject();
    await buttonNamed(refusedPage.get('tr.project-release'), 'Submit for approval').trigger('click');
    submitForm(refusedPage, '#project-publish-confirm');
    await flushPromises();
    expect(publish).not.toHaveBeenCalled();
    expect(refusedPage.get('[role="alert"]').text()).toBe("Only its owner and NovaWorld's admins may change that project.");
  });

  it('retires a published release and deletes a retired one behind a confirmation, and says why one stays', async () => {
    releases.mockResolvedValue({ ok: true, value: [nightOpsRelease(2, { status: 'published' }), nightOpsRelease(1, { status: 'retired' })] });
    retire.mockResolvedValueOnce({ ok: true, value: nightOpsRelease(2, { status: 'retired' }) });
    const page = await mountProject();
    const rows = () => page.findAll('tr.project-release');
    await buttonNamed(rows()[0], 'Retire').trigger('click');
    await flushPromises();
    expect(confirm).toHaveBeenLastCalledWith('Retire 0.2.0? Players stop getting it; running servers keep it.');
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
    expect(confirm).toHaveBeenLastCalledWith('Delete 0.1.0? Its package goes for good.');
    expect(page.get('[role="status"]').text()).toBe('0.1.0 is deleted.');
  });

  it('says a release gone is gone, and a quota refusal in its words', async () => {
    releases.mockResolvedValue({ ok: true, value: [nightOpsRelease(3)] });
    const page = await mountProject();
    publish.mockResolvedValueOnce({ ok: false, error: { code: 'not_found', status: 404, field: '', message: 'That project is not on NovaWorld, or it is private.' } });
    await buttonNamed(page.get('tr.project-release'), 'Submit for approval').trigger('click');
    submitForm(page, '#project-publish-confirm');
    await flushPromises();
    expect(page.get('[role="alert"]').text()).toBe('That release is no longer on NovaWorld.');
    expect(releases.mock.calls.length).toBeGreaterThan(1);

    const words = 'That would take your account past the 10 GiB it may store (9.9 GiB in use). Delete a release or a project you no longer need to make room.';
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
    expect(locked.find('#project-releases').exists()).toBe(false);
    expect(locked.find('#project-build-toggle').exists()).toBe(false);
    // Its history alone, with no tabs.
    expect(locked.find('.tab-nav').exists()).toBe(false);
    expect(locked.find('#project-history').exists()).toBe(true);
    expect(history).toHaveBeenCalledWith('LOCKED', undefined);
    wrapper?.unmount();

    const empty = await mountProject('RANGERS');
    expect(releases).toHaveBeenCalledWith('RANGERS', expect.anything());
    const releaseRows = empty.get('#project-releases').element.closest('section')!.querySelectorAll('tbody tr');
    expect([...releaseRows].map((tr) => tr.textContent!.trim())).toEqual(['No releases yet.']);
    expect(empty.find('#project-build-toggle').exists()).toBe(false);
  });

  it('says a release awaits approval and what players keep meanwhile, and withdraws it', async () => {
    releases.mockResolvedValue({ ok: true, value: [nightOpsRelease(3, { status: 'awaiting_approval' }), nightOpsRelease(2, { status: 'published' })] });
    removeRelease.mockResolvedValueOnce({ ok: true, value: true });
    const page = await mountProject();
    const row = page.get('tr.project-release');
    expect(row.get('.badge').text()).toBe('Awaiting approval');
    expect(row.get('.badge').attributes('title')).toBe('Players get it once an admin approves it.');
    expect(row.findAll('button').map((b) => b.text())).toEqual(['Withdraw release 0.3.0']);
    await buttonNamed(row, 'Withdraw').trigger('click');
    await flushPromises();
    expect(confirm).toHaveBeenLastCalledWith('Withdraw 0.3.0? It is deleted with its package.');
    expect(removeRelease).toHaveBeenCalledWith('NIGHTOPS', 3);
    expect(page.get('[role="status"]').text()).toBe('0.3.0 is withdrawn.');
  });

  it("shows the note an admin sent a draft back with, and an admin's own project publishes straight away", async () => {
    releases.mockResolvedValue({ ok: true, value: [nightOpsRelease(3, { review_note: 'The briefing names the wrong map.' })] });
    const page = await mountProject();
    expect(page.get('.project-release-note').text()).toBe('Sent back: The briefing names the wrong map.');
    wrapper?.unmount();

    session.role = 'admin';
    publish.mockResolvedValueOnce({ ok: true, value: nightOpsRelease(3, { status: 'published' }) });
    const own = await mountProject();
    await buttonNamed(own.get('tr.project-release'), 'Publish').trigger('click');
    expect(own.get('#project-publish-form p').text()).toBe('Publish 0.3.0 to players?');
    expect(own.get('#project-publish-confirm').text()).toBe('Publish');
    submitForm(own, '#project-publish-confirm');
    await flushPromises();
    expect(publish).toHaveBeenCalledWith('NIGHTOPS', 3);
    expect(own.get('[role="status"]').text()).toBe('0.3.0 is published.');
  });
});
