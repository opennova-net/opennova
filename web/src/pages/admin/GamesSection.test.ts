import { flushPromises, mount, type VueWrapper } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import type { Router } from 'vue-router';
import GamesSection from './GamesSection.vue';
import { createDataSet, fetchDataSets, fetchReleases } from '../../api/dataSets';
import { testRouter } from '../../test/router';
import { DFX2_SET, ESCALATION_SET, JO_SET, dataSet, dataSetsResponse, release, runtimeRelease } from '../../test/dataSetFixtures';

vi.mock('../../api/dataSets', () => ({
  createDataSet: vi.fn(),
  fetchDataSets: vi.fn(),
  fetchReleases: vi.fn(),
}));

const sets = vi.mocked(fetchDataSets);
const list = vi.mocked(fetchReleases);
const create = vi.mocked(createDataSet);

function submitForm(page: VueWrapper, selector: string) {
  return page.get(selector).element.closest('form')!.dispatchEvent(new Event('submit'));
}

describe('GamesSection', () => {
  let wrapper: VueWrapper | null = null;
  let router: Router;

  async function mountSection() {
    router = await testRouter('/admin/games');
    wrapper = mount(GamesSection, { global: { plugins: [router] } });
    await flushPromises();
    return wrapper;
  }

  beforeEach(() => {
    vi.clearAllMocks();
    sets.mockResolvedValue({ ok: true, value: dataSetsResponse() });
    list.mockResolvedValue({ ok: true, value: [release(10, { status: 'draft' }), release(9), runtimeRelease(7)] });
  });

  afterEach(() => {
    wrapper?.unmount();
    wrapper = null;
  });

  it('lists each game with its project, its published release and its servers, linking to its page', async () => {
    const page = await mountSection();
    const rows = page.findAll('tr.games-row');
    expect(rows.map((r) => r.get('th a').text())).toEqual(['Joint Operations', 'Escalation', 'Delta Force Xtreme 2', 'OpenNova runtime']);
    expect(rows.map((r) => r.get('th a').attributes('href'))).toEqual(['/admin/games/JO', '/admin/games/JOX01', '/admin/games/DFX2', '/admin/games/9']);
    expect(rows[0].text()).toContain('Base game Joint Operations, lobby Joint Operations');
    expect(rows[0].text()).toContain('Private, r2');
    expect(rows[0].text()).toContain('1.7.5.9');
    expect(rows[0].text()).toContain('3 live');
    expect(rows[0].text()).toContain('1 on an older release');
    expect(rows[1].text()).toContain('Expansion jox01 of Joint Operations');
    expect(rows[1].text()).toContain('nothing pushed');
    expect(rows[2].text()).toContain('None');
    expect(rows[3].text()).toContain('From CI');
    expect(rows[3].text()).toContain('v0.0.21');
    // No em dash in the page's copy, and no upload anywhere.
    expect(page.text()).not.toContain('—');
    expect(page.find('input[type="file"]').exists()).toBe(false);
  });

  it('lists the runtime from its releases when the games leave it out', async () => {
    sets.mockResolvedValue({ ok: true, value: dataSetsResponse({ data_sets: [JO_SET, ESCALATION_SET, DFX2_SET] }) });
    const page = await mountSection();
    const runtime = page.findAll('tr.games-row')[3];
    expect(runtime.get('th a').text()).toBe('OpenNova runtime');
    expect(runtime.text()).toContain('v0.0.21');
  });

  it('says why the games could not be read', async () => {
    sets.mockResolvedValue({ ok: false, error: { code: 'forbidden', status: 403, message: 'This account is not an admin.' } });
    const page = await mountSection();
    expect(page.get('[role="alert"]').text()).toBe('This account is not an admin.');
  });

  it('checks the new game form, sends the game with its project, and opens its page', async () => {
    create.mockResolvedValue({ ok: true, value: dataSet(5, { display_name: 'Escalation 2', kind: 'expansion', key: 'JOX02' }) });
    const page = await mountSection();
    await page.get('#games-new-toggle').trigger('click');
    expect((page.get('#games-new-title').element as HTMLSelectElement).value).toBe('jo');
    expect((page.get('#games-new-game').element as HTMLSelectElement).value).toBe('jop_2_consumer');

    // Nothing named, no key: refused in the page.
    submitForm(page, '#games-new-display_name');
    await flushPromises();
    expect(create).not.toHaveBeenCalled();
    expect(page.get('#games-new-display_name-error').text()).toBe('Enter a name.');
    expect(page.text()).toContain('A key is 1 to 16 of A-Z, 0-9 and _.');

    // An expansion: its base among the base games, its code's shape, the updater pair.
    await page.get('#games-new-kind').setValue('expansion');
    expect(page.findAll('#games-new-parent_id option').map((o) => o.text())).toEqual(['Joint Operations', 'Delta Force Xtreme 2']);
    await page.get('#games-new-display_name').setValue(' Escalation 2 ');
    await page.get('#games-new-key').setValue('jox02');
    await page.get('#games-new-expansion_code').setValue('JOX-02');
    await page.get('#games-new-updater_login').setValue('JOEus2');
    submitForm(page, '#games-new-display_name');
    await flushPromises();
    expect(create).not.toHaveBeenCalled();
    expect(page.get('#games-new-expansion_code-error').text()).toContain('1 to 8 lowercase letters and digits');
    expect(page.get('#games-new-command_file-error').text()).toBe('An updater login needs its command file.');

    await page.get('#games-new-expansion_code').setValue('jox02');
    await page.get('#games-new-command_file').setValue('joe2ptr.cmd');
    await page.get('#games-new-visibility').setValue('public');
    submitForm(page, '#games-new-display_name');
    await flushPromises();
    expect(create).toHaveBeenCalledWith({
      display_name: 'Escalation 2',
      kind: 'expansion',
      key: 'JOX02',
      visibility: 'public',
      retail_updater: false,
      updater_login: 'JOEus2',
      command_file: 'JOE2PTR.CMD',
      parent_id: 1,
      expansion_code: 'jox02',
    });
    expect(router.currentRoute.value.path).toBe('/admin/games/JOX02');
  });

  it("shows the service's refusal of a new game", async () => {
    create.mockResolvedValue({ ok: false, error: { code: 'duplicate_key', status: 409, message: "Another game's project already has that key." } });
    const page = await mountSection();
    await page.get('#games-new-toggle').trigger('click');
    await page.get('#games-new-display_name').setValue('Joint Operations');
    await page.get('#games-new-key').setValue('JO');
    submitForm(page, '#games-new-display_name');
    await flushPromises();
    expect(create).toHaveBeenCalledWith(expect.objectContaining({ kind: 'base', key: 'JO', title: 'jo', game: 'jop_2_consumer' }));
    expect(page.get('[role="alert"]').text()).toBe("Another game's project already has that key.");
    expect(page.find('#games-new-form').exists()).toBe(true);
  });
});
