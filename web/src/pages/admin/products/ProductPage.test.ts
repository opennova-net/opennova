import * as actionReview from '../../../composables/useConfirm';
import { flushPromises, mount, type VueWrapper } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi, type MockInstance } from 'vitest';
import { createMemoryHistory, createRouter, RouterView, type Router } from 'vue-router';
import ProductPage from './ProductPage.vue';
import {
  buildRelease,
  deleteProduct,
  deleteRelease,
  fetchProducts,
  fetchReleases,
  fetchRevisions,
  publishRelease,
  rejectRelease,
  retireRelease,
  updateAllServers,
  updateProduct,
} from '../../../api/products';
import { listUsers } from '../../../api/users';
import { userEntry } from '../../../test/adminFixtures';
import { testRouter } from '../../../test/router';
import { JO_PRODUCT, product, productsResponse, release, revision, runtimeRelease } from '../../../test/productFixtures';

vi.mock('../../../api/products', async (importOriginal) => ({
  REVISIONS_PAGE: (await importOriginal<typeof import('../../../api/products')>()).REVISIONS_PAGE,
  buildRelease: vi.fn(),
  deleteProduct: vi.fn(),
  deleteRelease: vi.fn(),
  fetchProducts: vi.fn(),
  fetchReleases: vi.fn(),
  fetchRevisions: vi.fn(),
  publishRelease: vi.fn(),
  rejectRelease: vi.fn(),
  retireRelease: vi.fn(),
  updateAllServers: vi.fn(),
  updateProduct: vi.fn(),
}));
vi.mock('../../../api/users', () => ({ listUsers: vi.fn() }));

const sets = vi.mocked(fetchProducts);
const list = vi.mocked(fetchReleases);
const history = vi.mocked(fetchRevisions);
const update = vi.mocked(updateProduct);
const remove = vi.mocked(deleteProduct);
const build = vi.mocked(buildRelease);
const publish = vi.mocked(publishRelease);
const reject = vi.mocked(rejectRelease);
const retire = vi.mocked(retireRelease);
const removeRelease = vi.mocked(deleteRelease);
const updateAll = vi.mocked(updateAllServers);
const users = vi.mocked(listUsers);

const NIGHT_OPS = product(21, {
  display_name: 'Night Ops',
  kind: 'expansion',
  parent_id: 1,
  expansion_code: 'nightops',
  title: '',
  lobby: '',
  key: 'NIGHTOPS',
  visibility: 'public',
  owner: 'biggy',
  owner_id: 7,
});

function submitForm(page: VueWrapper, selector: string) {
  return page.get(selector).element.closest('form')!.dispatchEvent(new Event('submit'));
}

function button(scope: { findAll: VueWrapper['findAll'] }, text: string) {
  return scope.findAll('button').find((b) => b.text().startsWith(text))!;
}

describe('ProductPage', () => {
  let wrapper: VueWrapper | null = null;
  let router: Router;
  let confirm: MockInstance<typeof actionReview.confirmAction>;

  async function mountProduct(ref = 'JO') {
    router = await testRouter(`/admin/products/${ref}`, ['/admin/products/:ref']);
    wrapper = mount(ProductPage, { global: { plugins: [router] } });
    await flushPromises();
    return wrapper;
  }

  const rows = () => wrapper!.findAll('tr.product-release');

  beforeEach(() => {
    vi.clearAllMocks();
    confirm = vi.spyOn(actionReview, 'confirmAction').mockResolvedValue(true);
    sets.mockResolvedValue({ ok: true, value: productsResponse() });
    list.mockResolvedValue({
      ok: true,
      value: [
        release(10, { status: 'building', sections: [], bytes: 0, sha256: '', build_log: '$ opennova-project export\n' }),
        release(9),
        release(8, { status: 'draft', created_by: 'user biggy' }),
        release(7, { status: 'retired', created_by: 'token' }),
        runtimeRelease(6),
        runtimeRelease(11, { status: 'uploading', product_id: null, sections: [] }),
      ],
    });
    history.mockResolvedValue({ ok: true, value: [revision(2), revision(1)] });
    users.mockResolvedValue({ ok: true, value: [userEntry(1, { username: 'root' }), userEntry(7, { username: 'biggy' })] });
  });

  afterEach(() => {
    wrapper?.unmount();
    wrapper = null;
    confirm.mockRestore();
  });

  it('heads the page with its crumb, facts and actions, then its tiles, project and releases', async () => {
    const page = await mountProduct();
    expect(page.get('h1').text()).toBe('Joint Operations');
    const crumbs = page.get('nav[aria-label="Breadcrumb"]');
    expect(crumbs.findAll('a').map((a) => [a.text(), a.attributes('href')])).toEqual([['Products', '/admin/products']]);
    const meta = page.get('.page-meta');
    expect(meta.get('.product-kind').text()).toBe('Game Joint Operations, lobby Joint Operations');
    expect(meta.get('.product-protocol').text()).toBe('JO protocol 16');
    expect(meta.text()).toContain('JO');
    expect(meta.get('.product-owner').text()).toBe('OpenNova');
    expect(meta.get('.product-visibility').text()).toBe('Private');
    expect(page.get('#product-edit-toggle').text()).toBe('Edit…');
    expect(page.get('#product-delete-toggle').text()).toBe('Delete…');

    // The tiles: what servers run, the live ones (to the servers list), the outdated.
    const tiles = page.findAll('.stat-tile');
    expect(tiles.map((t) => t.get('dt').text())).toEqual(['Server release', 'Live servers', 'Outdated']);
    expect(tiles[0].text()).toContain('1.7.5.9');
    expect(tiles[1].get('a').attributes('href')).toBe('/admin/servers?product=1');
    expect(tiles[2].classes()).toContain('border-warn/40');
    expect(page.get('#product-update-servers').text()).toBe('Move 1 to 1.7.5.9');

    expect(history).toHaveBeenCalledWith('JO', undefined);
    expect(page.get('#product-project-line').text()).toMatch(/^r2 by biggy: weapon tweaks$/);
    expect(page.find('nav[aria-label="Product details"]').exists()).toBe(false);
    expect(page.text()).not.toContain('—');
  });

  it('lists its releases with their state, package, creator and actions', async () => {
    const page = await mountProduct();
    const table = page.get('table');
    expect(table.findAll('thead th').map((th) => th.text())).toEqual(['Release', 'Status', 'Package', 'Made', 'Actions']);
    expect(rows().map((r) => r.get('.product-release-version').text())).toEqual(['1.7.5.10', '1.7.5.9', '1.7.5.8', '1.7.5.7']);
    expect(rows()[0].get('.product-release-one').text()).toBe('ONE 0.0.22');
    expect(rows()[0].text()).toContain('(r2)');
    expect(rows()[0].text()).toContain('Building');
    expect(rows()[0].text()).toContain('Not packed yet');
    expect(rows()[0].find('details').attributes('open')).toBeDefined();
    expect(rows()[1].text()).toContain('Published');
    expect(rows()[1].text()).toContain("Servers' data ready");
    expect(rows()[1].findAll('button').map((b) => b.text())).toEqual(['Retire release 1.7.5.9']);
    expect(rows()[2].findAll('button').map((b) => b.text())).toEqual(['Publish release 1.7.5.8', 'Delete release 1.7.5.8']);
    expect(rows()[3].findAll('button').map((b) => b.text())).toEqual(['Delete release 1.7.5.7']);

    // Who started each: an account, linked by its id; the admin token in words.
    expect(users).toHaveBeenCalledTimes(1);
    const creators = rows().map((r) => r.get('.product-release-creator'));
    expect(creators[1].text()).toBe('by admin root');
    expect(creators[1].get('a').attributes('href')).toBe('/admin/players/1');
    expect(creators[2].text()).toBe('by biggy');
    expect(creators[2].get('a').attributes('href')).toBe('/admin/players/7');
    expect(creators[3].text()).toBe('by the admin token');
    expect(creators[3].find('a').exists()).toBe(false);
  });

  it("links a creator who owns the product by the product's owner id, and names one it cannot find", async () => {
    users.mockResolvedValue({ ok: false, error: { code: 'network', status: null, message: 'Could not reach the server.' } });
    sets.mockResolvedValue({ ok: true, value: productsResponse({ products: [JO_PRODUCT, NIGHT_OPS] }) });
    list.mockResolvedValue({
      ok: true,
      value: [
        release(4, { product_id: 21, status: 'draft', version: '0.4.0', created_by: 'user biggy', sections: [] }),
        release(3, { product_id: 21, status: 'draft', version: '0.3.0', created_by: 'admin root', sections: [] }),
      ],
    });
    const page = await mountProduct('NIGHTOPS');
    const [owner, admin] = rows().map((r) => r.get('.product-release-creator'));
    expect(owner.get('a').attributes('href')).toBe('/admin/players/7');
    expect(admin.text()).toBe('by admin root');
    expect(admin.find('a').exists()).toBe(false);
    expect(page.find('[role="alert"]').text()).toBe('');
  });

  it("opens the project's history", async () => {
    const page = await mountProduct();
    expect(page.find('#product-history').exists()).toBe(false);
    await page.get('#product-history-toggle').trigger('click');
    const revisions = page.findAll('li.product-revision');
    expect(revisions).toHaveLength(2);
    expect(revisions[0].text()).toContain('r2');
    expect(revisions[0].text()).toContain('weapon tweaks');
    expect(revisions[1].text()).toContain('biggy, 1200 files, 1.2 GB');
  });

  it("shows a player's product with its owner linked and nothing pushed", async () => {
    sets.mockResolvedValue({ ok: true, value: productsResponse({ products: [JO_PRODUCT, NIGHT_OPS] }) });
    const page = await mountProduct('NIGHTOPS');
    const owner = page.get('.page-meta .product-owner a');
    expect([owner.text(), owner.attributes('href')]).toEqual(['biggy', '/admin/players/7']);
    expect(page.get('.product-visibility').text()).toBe('Public');
    expect(page.get('#product-project-line').text()).toBe('Nothing pushed yet.');
    expect(page.find('#product-history-toggle').exists()).toBe(false);
    expect(page.find('#product-build-toggle').exists()).toBe(false);
    expect(history).not.toHaveBeenCalled();
    expect(page.text()).toContain('No releases yet.');
  });

  it("changes the project's visibility and renames its key, following it to its new address", async () => {
    update.mockResolvedValueOnce({ ok: true, value: { ...JO_PRODUCT, visibility: 'public' } });
    const page = await mountProduct();
    await page.get('#product-visibility').trigger('click');
    await flushPromises();
    expect(update).toHaveBeenCalledWith(1, { visibility: 'public' });
    expect(page.get('[role="status"]').text()).toBe('JO is public.');

    await page.get('#product-rename-toggle').trigger('click');
    await page.get('#product-rename-key').setValue('j o');
    submitForm(page, '#product-rename-key');
    await flushPromises();
    expect(update).toHaveBeenCalledTimes(1);
    expect(page.get('#product-rename-form').text()).toContain('A key is 1 to 16 of A-Z, 0-9 and _.');

    update.mockResolvedValueOnce({ ok: true, value: { ...JO_PRODUCT, key: 'JOP' } });
    await page.get('#product-rename-key').setValue('jop');
    submitForm(page, '#product-rename-key');
    await flushPromises();
    expect(update).toHaveBeenLastCalledWith(1, { key: 'JOP' });
    expect(router.currentRoute.value.path).toBe('/admin/products/JOP');
    expect(page.get('h1').text()).toBe('Joint Operations');
    expect(page.get('[role="status"]').text()).toBe('JO is now JOP.');
  });

  it("edits the product's settings", async () => {
    update.mockResolvedValue({ ok: true, value: { ...JO_PRODUCT, display_name: 'JO' } });
    const page = await mountProduct();
    await page.get('#product-edit-toggle').trigger('click');
    expect(page.findAll('#product-edit-lobby option').map((option) => option.attributes('value'))).toEqual(['jop_2_consumer', 'acidgambit_0_consumer']);
    await page.get('#product-edit-command_file').setValue('');
    submitForm(page, '#product-edit-display_name');
    await flushPromises();
    expect(update).not.toHaveBeenCalled();
    expect(page.get('#product-edit-error').text()).toBe('An updater login needs its command file.');

    await page.get('#product-edit-display_name').setValue('JO');
    await page.get('#product-edit-command_file').setValue('joptr.cmd');
    submitForm(page, '#product-edit-display_name');
    await flushPromises();
    expect(update).toHaveBeenCalledWith(1, { display_name: 'JO', lobby: 'jop_2_consumer', updater_login: 'JOTRus', command_file: 'JOPTR.CMD', retail: true });
    expect(page.get('[role="status"]').text()).toBe('JO is saved.');
    expect(page.find('#product-edit-form').exists()).toBe(false);
  });

  it('keeps a locked lobby read-only while saving other settings', async () => {
    const locked = { ...JO_PRODUCT, lobby_locked: true };
    sets.mockResolvedValue({ ok: true, value: productsResponse({ products: [locked] }) });
    update.mockResolvedValue({ ok: true, value: locked });
    const page = await mountProduct();
    await page.get('#product-edit-toggle').trigger('click');
    expect(page.get('#product-edit-lobby').attributes('disabled')).toBeDefined();
    await page.get('#product-edit-display_name').setValue('Joint Ops');
    submitForm(page, '#product-edit-form');
    await flushPromises();
    expect(update).toHaveBeenCalledWith(1, { display_name: 'Joint Ops', updater_login: 'JOTRus', command_file: 'JOPTR.CMD', retail: true });
  });

  it('builds a release from a revision once its version reads right', async () => {
    build.mockResolvedValue({ ok: true, value: release(12, { status: 'building', version: '1.7.5.12' }) });
    const page = await mountProduct();
    expect(page.find('#product-build-form').exists()).toBe(false);
    await page.get('#product-build-toggle').trigger('click');
    expect((page.get('#product-build-revision').element as HTMLInputElement).value).toBe('2');
    await page.get('#product-build-version').setValue('1.7.5.x');
    submitForm(page, '#product-build-version');
    await flushPromises();
    expect(build).not.toHaveBeenCalled();
    expect(page.get('#product-build-form').text()).toContain('A game version is one to four numbers between dots.');

    await page.get('#product-build-version').setValue('1.7.5.12');
    submitForm(page, '#product-build-version');
    await flushPromises();
    expect(build).toHaveBeenCalledWith('JO', { version: '1.7.5.12', revision: 2 });
    expect(page.get('[role="status"]').text()).toBe('1.7.5.12 is building.');
  });

  it('publishes, retires and deletes a release, each behind a one-sentence confirm', async () => {
    publish.mockResolvedValue({ ok: true, value: release(8) });
    retire.mockResolvedValue({ ok: false, error: { code: 'not_published', status: 409, message: 'That release is not published. Refresh the list.' } });
    removeRelease.mockResolvedValue({ ok: true, value: true });
    const page = await mountProduct();

    await button(rows()[2], 'Publish').trigger('click');
    await flushPromises();
    expect(confirm).toHaveBeenLastCalledWith('Publish 1.7.5.8? Players get it and new servers run it.');
    expect(publish).toHaveBeenCalledWith(8);
    expect(page.get('[role="status"]').text()).toBe('1.7.5.8 is published.');

    await button(rows()[1], 'Retire').trigger('click');
    await flushPromises();
    expect(confirm).toHaveBeenLastCalledWith('Retire 1.7.5.9? Players stop getting it.');
    expect(retire).toHaveBeenCalledWith(9);
    expect(page.get('[role="alert"]').text()).toBe('That release is not published. Refresh the list.');

    confirm.mockResolvedValueOnce(false);
    await button(rows()[3], 'Delete').trigger('click');
    await flushPromises();
    expect(removeRelease).not.toHaveBeenCalled();
    await button(rows()[3], 'Delete').trigger('click');
    await flushPromises();
    expect(removeRelease).toHaveBeenCalledWith(7);
    expect(page.get('[role="status"]').text()).toBe('1.7.5.7 is deleted.');
  });

  it('moves the outdated servers to the published release', async () => {
    updateAll.mockResolvedValue({ ok: true, value: 1 });
    const page = await mountProduct();
    await page.get('#product-update-servers').trigger('click');
    await flushPromises();
    expect(confirm).toHaveBeenCalledWith('Move 1 server of Joint Operations to 1.7.5.9? Each restarts and drops its players.');
    expect(updateAll).toHaveBeenCalledWith(1);
    expect(page.get('[role="status"]').text()).toBe('1 server is moving to 1.7.5.9.');
  });

  it('deletes the product behind its key typed, and shows why one stays', async () => {
    remove.mockResolvedValueOnce({ ok: false, error: { code: 'has_expansion', status: 409, message: 'An expansion builds on that game. Delete the expansion first.' } });
    const page = await mountProduct();
    await page.get('#product-delete-toggle').trigger('click');
    const submit = page.get('#product-delete-form button[type="submit"]');
    expect(submit.attributes('disabled')).toBeDefined();
    await page.get('#product-delete-confirm').setValue('JO');
    expect(submit.attributes('disabled')).toBeUndefined();
    submitForm(page, '#product-delete-confirm');
    await flushPromises();
    expect(remove).toHaveBeenCalledWith(1);
    expect(page.get('[role="alert"]').text()).toBe('An expansion builds on that game. Delete the expansion first.');

    remove.mockResolvedValueOnce({ ok: true, value: true });
    submitForm(page, '#product-delete-confirm');
    await flushPromises();
    expect(router.currentRoute.value.path).toBe('/admin/products');
  });

  it("shows the runtime's releases alone, its upload not yet read among them", async () => {
    const page = await mountProduct('9');
    expect(page.get('h1').text()).toBe('OpenNova runtime');
    expect(page.find('.stat-tile').exists()).toBe(false);
    expect(page.find('#product-project').exists()).toBe(false);
    expect(page.find('#product-edit-toggle').exists()).toBe(false);
    expect(page.find('#product-delete-toggle').exists()).toBe(false);
    expect(page.find('#product-build-toggle').exists()).toBe(false);
    expect(page.find('.product-visibility').exists()).toBe(false);
    expect(history).not.toHaveBeenCalled();
    expect(rows().map((r) => r.get('.product-release-version').text())).toEqual(['v0.0.21', 'v0.0.21']);
    expect(rows()[0].text()).toContain('Published');
    expect(rows()[1].text()).toContain('Uploading');
    expect(button(page.get('tbody'), 'Retire').exists()).toBe(true);
  });

  it.each(['rename', 'build'] as const)('leaves a deleted product without warning about its %s draft', async (draft) => {
    router = createRouter({
      history: createMemoryHistory(),
      routes: [
        { path: '/admin/products/:ref', component: ProductPage },
        { path: '/:rest(.*)*', component: { render: () => null } },
      ],
    });
    await router.push('/admin/products/JO');
    wrapper = mount(RouterView, { global: { plugins: [router] } });
    await flushPromises();
    await wrapper.get(`#product-${draft}-toggle`).trigger('click');
    await wrapper.get(draft === 'rename' ? '#product-rename-key' : '#product-build-version').setValue(draft === 'rename' ? 'JOP' : '1.7.6');
    confirm.mockResolvedValue(false);
    await router.push('/admin/products');
    expect(router.currentRoute.value.path).toBe('/admin/products/JO');
    expect(confirm).toHaveBeenCalledWith(expect.stringContaining('unsaved changes'), 'Unsaved changes');

    confirm.mockClear();
    remove.mockResolvedValueOnce({ ok: true, value: true });
    await wrapper.get('#product-delete-toggle').trigger('click');
    await wrapper.get('#product-delete-confirm').setValue('JO');
    submitForm(wrapper, '#product-delete-confirm');
    await flushPromises();
    expect(remove).toHaveBeenCalledWith(1);
    expect(router.currentRoute.value.path).toBe('/admin/products');
    expect(confirm).not.toHaveBeenCalled();
  });

  it('says when no product has the address', async () => {
    const page = await mountProduct('NOPE');
    expect(page.get('h1').text()).toBe('No such product');
    expect(page.get('.product-missing').text()).toContain('No product has the key or id NOPE.');
    expect(page.get('.product-missing a').attributes('href')).toBe('/admin/products');
  });

  it("approves or rejects a player's release awaiting approval, the rejection with a note", async () => {
    sets.mockResolvedValue({ ok: true, value: productsResponse({ products: [JO_PRODUCT, { ...NIGHT_OPS, head: 3 }] }) });
    const waiting = (id: number) => release(id, { product_id: 21, status: 'awaiting_approval', version: `0.${id}.0`, created_by: 'user biggy', sections: [] });
    list.mockResolvedValue({ ok: true, value: [waiting(4), waiting(3), release(2, { product_id: 21, status: 'draft', version: '0.2.0', sections: [], review_note: 'Wrong map.' })] });
    const page = await mountProduct('NIGHTOPS');
    expect(rows()[0].text()).toContain('Awaiting approval');
    expect(rows()[0].findAll('button').map((b) => b.text())).toEqual(['Approve release 0.4.0', 'Reject… release 0.4.0']);
    expect(rows()[2].get('.product-release-note').text()).toBe('Sent back: Wrong map.');

    publish.mockResolvedValueOnce({ ok: true, value: { ...waiting(4), status: 'published' } });
    await button(rows()[0], 'Approve').trigger('click');
    await flushPromises();
    expect(confirm).toHaveBeenLastCalledWith('Approve 0.4.0? Players get it and new servers run it.');
    expect(publish).toHaveBeenCalledWith(4);
    expect(page.get('[role="status"]').text()).toBe('0.4.0 is approved and published.');

    reject.mockResolvedValueOnce({ ok: true, value: { ...waiting(3), status: 'draft', review_note: 'Fix the briefing.' } });
    await button(rows()[1], 'Reject').trigger('click');
    expect(page.get('#product-reject-form').text()).toContain('Send 0.3.0 back to biggy');
    await page.get('#product-reject-note').setValue('  Fix the briefing. ');
    submitForm(page, '#product-reject-note');
    await flushPromises();
    expect(reject).toHaveBeenCalledWith(3, 'Fix the briefing.');
    expect(page.get('[role="status"]').text()).toBe('0.3.0 is back with its owner as a draft.');
    expect(page.find('#product-reject-form').exists()).toBe(false);
  });
});
