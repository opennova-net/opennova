import { flushPromises, mount, type VueWrapper } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi, type MockInstance } from 'vitest';
import type { Router } from 'vue-router';
import GameSection from './GameSection.vue';
import {
  buildRelease,
  deleteDataSet,
  deleteRelease,
  fetchDataSets,
  fetchReleases,
  fetchRevisions,
  publishRelease,
  retireRelease,
  updateAllServers,
  updateDataSet,
} from '../../api/dataSets';
import { testRouter } from '../../test/router';
import { JO_SET, dataSetsResponse, release, revision, runtimeRelease } from '../../test/dataSetFixtures';

vi.mock('../../api/dataSets', async (importOriginal) => ({
  REVISIONS_PAGE: (await importOriginal<typeof import('../../api/dataSets')>()).REVISIONS_PAGE,
  buildRelease: vi.fn(),
  deleteDataSet: vi.fn(),
  deleteRelease: vi.fn(),
  fetchDataSets: vi.fn(),
  fetchReleases: vi.fn(),
  fetchRevisions: vi.fn(),
  publishRelease: vi.fn(),
  retireRelease: vi.fn(),
  updateAllServers: vi.fn(),
  updateDataSet: vi.fn(),
}));

const sets = vi.mocked(fetchDataSets);
const list = vi.mocked(fetchReleases);
const history = vi.mocked(fetchRevisions);
const update = vi.mocked(updateDataSet);
const remove = vi.mocked(deleteDataSet);
const build = vi.mocked(buildRelease);
const publish = vi.mocked(publishRelease);
const retire = vi.mocked(retireRelease);
const removeRelease = vi.mocked(deleteRelease);
const updateAll = vi.mocked(updateAllServers);

function submitForm(page: VueWrapper, selector: string) {
  return page.get(selector).element.closest('form')!.dispatchEvent(new Event('submit'));
}

function buttonNamed(page: VueWrapper, scope: string, text: string) {
  return page.get(scope).findAll('button').find((b) => b.text().startsWith(text))!;
}

describe('GameSection', () => {
  let wrapper: VueWrapper | null = null;
  let router: Router;
  let confirm: MockInstance<typeof window.confirm>;

  async function mountGame(ref = 'JO') {
    router = await testRouter(`/admin/games/${ref}`, ['/admin/games/:ref']);
    wrapper = mount(GameSection, { global: { plugins: [router] } });
    await flushPromises();
    return wrapper;
  }

  beforeEach(() => {
    vi.clearAllMocks();
    confirm = vi.spyOn(window, 'confirm').mockReturnValue(true);
    sets.mockResolvedValue({ ok: true, value: dataSetsResponse() });
    list.mockResolvedValue({
      ok: true,
      value: [
        release(10, { status: 'building', sections: [], bytes: 0, sha256: '', build_log: '$ opennova-project export\n' }),
        release(9),
        release(8, { status: 'draft' }),
        release(7, { status: 'retired' }),
        runtimeRelease(6),
        runtimeRelease(11, { status: 'uploading', data_set_id: null, sections: [] }),
      ],
    });
    history.mockResolvedValue({ ok: true, value: [revision(2), revision(1)] });
  });

  afterEach(() => {
    wrapper?.unmount();
    wrapper = null;
    confirm.mockRestore();
  });

  it('shows the project, its releases and its servers', async () => {
    const page = await mountGame();
    expect(page.get('h1').text()).toBe('Joint Operations');
    expect(page.text()).toContain('Base game Joint Operations, lobby Joint Operations');
    expect(history).toHaveBeenCalledWith('JO', undefined);
    expect(page.get('#game-project-line').text()).toMatch(/^JO, private: admins only\.\s+Newest r2 by biggy: weapon tweaks\.$/);

    const rows = page.findAll('tr.game-release');
    expect(rows.map((r) => r.get('th span').text())).toEqual(['1.7.5.10', '1.7.5.9', '1.7.5.8', '1.7.5.7']);
    expect(rows[0].text()).toContain('(r2)');
    expect(rows[0].text()).toContain('Building');
    expect(rows[0].text()).toContain('Not packed yet');
    expect(rows[0].find('details').attributes('open')).toBeDefined();
    expect(rows[1].text()).toContain('Published');
    expect(rows[1].text()).toContain("Servers' data ready");
    expect(rows[1].findAll('button').map((b) => b.text())).toEqual(['Retire release 1.7.5.9']);
    expect(rows[2].findAll('button').map((b) => b.text())).toEqual(['Publish release 1.7.5.8', 'Delete release 1.7.5.8']);
    expect(rows[3].findAll('button').map((b) => b.text())).toEqual(['Delete release 1.7.5.7']);

    expect(page.get('#game-servers-line').text()).toBe('3 live servers. New servers start on 1.7.5.9.');
    expect(page.get('#game-update-servers').text()).toBe('Move it to 1.7.5.9');
    expect(page.text()).not.toContain('—');
  });

  it("opens the project's history", async () => {
    const page = await mountGame();
    expect(page.find('#game-history').exists()).toBe(false);
    await page.get('#game-history-toggle').trigger('click');
    const revisions = page.findAll('li.game-revision');
    expect(revisions).toHaveLength(2);
    expect(revisions[0].text()).toContain('r2');
    expect(revisions[0].text()).toContain('weapon tweaks');
    expect(revisions[1].text()).toContain('Stock import');
    expect(revisions[1].text()).toContain('biggy, 1200 files, 1.2 GB');
  });

  it("changes the project's visibility and renames its key, following it to its new address", async () => {
    update.mockResolvedValueOnce({ ok: true, value: { ...JO_SET, visibility: 'public' } });
    const page = await mountGame();
    await page.get('#game-visibility').trigger('click');
    await flushPromises();
    expect(update).toHaveBeenCalledWith(1, { visibility: 'public' });
    expect(page.get('[role="status"]').text()).toBe('Anyone may now get JO.');

    await page.get('#game-rename-toggle').trigger('click');
    await page.get('#game-rename-key').setValue('j o');
    submitForm(page, '#game-rename-key');
    await flushPromises();
    expect(update).toHaveBeenCalledTimes(1);
    expect(page.get('#game-rename-form').text()).toContain('A key is 1 to 16 of A-Z, 0-9 and _.');

    update.mockResolvedValueOnce({ ok: true, value: { ...JO_SET, key: 'JOP' } });
    await page.get('#game-rename-key').setValue('jop');
    submitForm(page, '#game-rename-key');
    await flushPromises();
    expect(update).toHaveBeenLastCalledWith(1, { key: 'JOP' });
    expect(router.currentRoute.value.path).toBe('/admin/games/JOP');
    expect(page.get('h1').text()).toBe('Joint Operations');
    expect(page.get('[role="status"]').text()).toBe('JO is now JOP.');
  });

  it("edits the game's settings", async () => {
    update.mockResolvedValue({ ok: true, value: { ...JO_SET, display_name: 'JO' } });
    const page = await mountGame();
    await page.get('#game-edit-toggle').trigger('click');
    await page.get('#game-edit-command_file').setValue('');
    submitForm(page, '#game-edit-display_name');
    await flushPromises();
    expect(update).not.toHaveBeenCalled();
    expect(page.get('#game-edit-error').text()).toBe('An updater login needs its command file.');

    await page.get('#game-edit-display_name').setValue('JO');
    await page.get('#game-edit-command_file').setValue('joptr.cmd');
    submitForm(page, '#game-edit-display_name');
    await flushPromises();
    expect(update).toHaveBeenCalledWith(1, { display_name: 'JO', game: 'jop_2_consumer', updater_login: 'JOTRus', command_file: 'JOPTR.CMD', retail_updater: true });
    expect(page.get('[role="status"]').text()).toBe('JO is saved.');
    expect(page.find('#game-edit-form').exists()).toBe(false);
  });

  it('builds a release from a revision once its version reads right', async () => {
    build.mockResolvedValue({ ok: true, value: release(12, { status: 'building', version: '1.7.5.12' }) });
    const page = await mountGame();
    await page.get('#game-build-toggle').trigger('click');
    expect((page.get('#game-build-revision').element as HTMLInputElement).value).toBe('2');
    await page.get('#game-build-version').setValue('1.7.5.x');
    submitForm(page, '#game-build-version');
    await flushPromises();
    expect(build).not.toHaveBeenCalled();
    expect(page.get('#game-build-form').text()).toContain('A version is one to four numbers between dots.');

    await page.get('#game-build-version').setValue('1.7.5.12');
    submitForm(page, '#game-build-version');
    await flushPromises();
    expect(build).toHaveBeenCalledWith('JO', { version: '1.7.5.12', revision: 2 });
    expect(page.get('[role="status"]').text()).toBe('1.7.5.12 is building: a draft to publish here once it is checked.');
  });

  it('publishes, retires and deletes a release, each behind a confirm', async () => {
    publish.mockResolvedValue({ ok: true, value: release(8) });
    retire.mockResolvedValue({ ok: false, error: { code: 'not_published', status: 409, message: 'That release is not published. Refresh the list.' } });
    removeRelease.mockResolvedValue({ ok: true, value: true });
    const page = await mountGame();
    const rows = () => page.findAll('tr.game-release');

    await rows()[2].findAll('button').find((b) => b.text().startsWith('Publish'))!.trigger('click');
    await flushPromises();
    expect(confirm).toHaveBeenLastCalledWith(expect.stringContaining('Publish 1.7.5.8?'));
    expect(publish).toHaveBeenCalledWith(8);
    expect(page.get('[role="status"]').text()).toBe('1.7.5.8 is published.');

    await rows()[1].findAll('button').find((b) => b.text().startsWith('Retire'))!.trigger('click');
    await flushPromises();
    expect(retire).toHaveBeenCalledWith(9);
    expect(page.get('[role="alert"]').text()).toBe('That release is not published. Refresh the list.');

    confirm.mockReturnValueOnce(false);
    await rows()[3].findAll('button').find((b) => b.text().startsWith('Delete'))!.trigger('click');
    await flushPromises();
    expect(removeRelease).not.toHaveBeenCalled();
    await rows()[3].findAll('button').find((b) => b.text().startsWith('Delete'))!.trigger('click');
    await flushPromises();
    expect(removeRelease).toHaveBeenCalledWith(7);
    expect(page.get('[role="status"]').text()).toBe('1.7.5.7 is deleted.');
  });

  it('moves the outdated servers to the published release', async () => {
    updateAll.mockResolvedValue({ ok: true, value: 1 });
    const page = await mountGame();
    await page.get('#game-update-servers').trigger('click');
    await flushPromises();
    expect(confirm).toHaveBeenCalledWith(expect.stringContaining('Move the 1 server of Joint Operations on an older release to 1.7.5.9?'));
    expect(updateAll).toHaveBeenCalledWith(1);
    expect(page.get('[role="status"]').text()).toBe('1 server is moving to 1.7.5.9.');
  });

  it('deletes the game behind its key typed, and says why one stays', async () => {
    remove.mockResolvedValueOnce({ ok: false, error: { code: 'has_expansion', status: 409, message: 'An expansion builds on that game. Delete the expansion first.' } });
    const page = await mountGame();
    await page.get('#game-delete-toggle').trigger('click');
    const submit = page.get('#game-delete-form button[type="submit"]');
    expect(submit.attributes('disabled')).toBeDefined();
    await page.get('#game-delete-confirm').setValue('JO');
    expect(submit.attributes('disabled')).toBeUndefined();
    submitForm(page, '#game-delete-confirm');
    await flushPromises();
    expect(remove).toHaveBeenCalledWith(1);
    expect(page.get('[role="alert"]').text()).toBe('An expansion builds on that game. Delete the expansion first.');

    remove.mockResolvedValueOnce({ ok: true, value: true });
    submitForm(page, '#game-delete-confirm');
    await flushPromises();
    expect(router.currentRoute.value.path).toBe('/admin/games');
  });

  it("shows the runtime's releases alone, its upload not yet read among them", async () => {
    const page = await mountGame('9');
    expect(page.get('h1').text()).toBe('OpenNova runtime');
    expect(page.find('#game-project').exists()).toBe(false);
    expect(page.find('#game-servers').exists()).toBe(false);
    expect(page.find('#game-edit-toggle').exists()).toBe(false);
    expect(page.find('#game-delete-toggle').exists()).toBe(false);
    expect(page.find('#game-build-toggle').exists()).toBe(false);
    expect(page.text()).toContain('Published by the Publish workflow');
    expect(history).not.toHaveBeenCalled();
    const rows = page.findAll('tr.game-release');
    expect(rows.map((r) => r.find('th span').text())).toEqual(['v0.0.21', 'v0.0.21']);
    expect(rows[0].text()).toContain('Published');
    expect(rows[1].text()).toContain('Uploading');
    expect(buttonNamed(page, 'tbody', 'Retire').exists()).toBe(true);
  });

  it('says when no game has the address', async () => {
    const page = await mountGame('NOPE');
    expect(page.get('h1').text()).toBe('No such game');
    expect(page.text()).toContain('No game has the key or id NOPE.');
  });
});
