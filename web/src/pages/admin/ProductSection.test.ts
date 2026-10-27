import * as actionReview from '../../composables/useConfirm';
import { flushPromises, mount, type VueWrapper } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi, type MockInstance } from 'vitest';
import { createMemoryHistory, createRouter, RouterView, type Router } from 'vue-router';
import ProductSection from './ProductSection.vue';
import {
  buildRelease,
  deleteProduct,
  deleteRelease,
  fetchProducts,
  fetchReleases,
  fetchRevisions,
  publishRelease,
  retireRelease,
  updateAllServers,
  updateProduct,
} from '../../api/products';
import { testRouter } from '../../test/router';
import { JO_PRODUCT, product, productsResponse, release, revision, runtimeRelease } from '../../test/productFixtures';

vi.mock('../../api/products', async (importOriginal) => ({
  REVISIONS_PAGE: (await importOriginal<typeof import('../../api/products')>()).REVISIONS_PAGE,
  buildRelease: vi.fn(),
  deleteProduct: vi.fn(),
  deleteRelease: vi.fn(),
  fetchProducts: vi.fn(),
  fetchReleases: vi.fn(),
  fetchRevisions: vi.fn(),
  publishRelease: vi.fn(),
  retireRelease: vi.fn(),
  updateAllServers: vi.fn(),
  updateProduct: vi.fn(),
}));

const sets = vi.mocked(fetchProducts);
const list = vi.mocked(fetchReleases);
const history = vi.mocked(fetchRevisions);
const update = vi.mocked(updateProduct);
const remove = vi.mocked(deleteProduct);
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

describe('ProductSection', () => {
  let wrapper: VueWrapper | null = null;
  let router: Router;
  let confirm: MockInstance<typeof actionReview.confirmAction>;

  async function mountProduct(ref = 'JO') {
    router = await testRouter(`/admin/products/${ref}`, ['/admin/products/:ref']);
    wrapper = mount(ProductSection, { global: { plugins: [router] } });
    await flushPromises();
    return wrapper;
  }

  beforeEach(() => {
    vi.clearAllMocks();
    confirm = vi.spyOn(actionReview, 'confirmAction').mockResolvedValue(true);
    sets.mockResolvedValue({ ok: true, value: productsResponse() });
    list.mockResolvedValue({
      ok: true,
      value: [
        release(10, { status: 'building', sections: [], bytes: 0, sha256: '', build_log: '$ opennova-project export\n' }),
        release(9),
        release(8, { status: 'draft' }),
        release(7, { status: 'retired' }),
        runtimeRelease(6),
        runtimeRelease(11, { status: 'uploading', product_id: null, sections: [] }),
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
    const page = await mountProduct();
    expect(page.get('h1').text()).toBe('Joint Operations');
    expect(page.text()).toContain('Game Joint Operations, lobby Joint Operations');
    expect(history).toHaveBeenCalledWith('JO', undefined);
    expect(page.get('#product-owner-line').text()).toBe('Owner: OpenNova');
    expect(page.get('#product-project-line').text()).toMatch(/^JO, private: admins only\.\s+Newest r2 by biggy: weapon tweaks\.$/);

    const rows = page.findAll('tr.product-release');
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

    expect(page.findAll('a').find((a) => a.text() === 'View these servers in the fleet')?.attributes('href')).toBe('/admin/fleet?game=1');
    expect(page.get('#product-servers-line').text()).toBe('3 live servers. New servers start on 1.7.5.9.');
    expect(page.get('#product-update-servers').text()).toBe('Move it to 1.7.5.9');
    expect(page.text()).not.toContain('—');
  });

  it("opens the project's history", async () => {
    const page = await mountProduct();
    expect(page.find('#product-history').exists()).toBe(false);
    await page.get('#product-history-toggle').trigger('click');
    const revisions = page.findAll('li.product-revision');
    expect(revisions).toHaveLength(2);
    expect(revisions[0].text()).toContain('r2');
    expect(revisions[0].text()).toContain('weapon tweaks');
    expect(revisions[1].text()).toContain('Stock import');
    expect(revisions[1].text()).toContain('biggy, 1200 files, 1.2 GB');
  });

  it("shows a player's product with its owner, nothing pushed, and what a rename does to editors", async () => {
    const nightOps = product(21, { display_name: 'Night Ops', kind: 'expansion', parent_id: 1, expansion_code: 'nightops', title: '', lobby: '', key: 'NIGHTOPS', visibility: 'public', owner: 'biggy', owner_id: 7 });
    sets.mockResolvedValue({ ok: true, value: productsResponse({ products: [JO_PRODUCT, nightOps] }) });
    update.mockResolvedValueOnce({ ok: true, value: { ...nightOps, visibility: 'private' } });
    const page = await mountProduct('NIGHTOPS');
    const owner = page.get('#product-owner-line a');
    expect([owner.text(), owner.attributes('href')]).toEqual(['biggy', '/admin/users/7']);
    expect(page.get('#product-project-line').text()).toMatch(/^NIGHTOPS,\s+public: anyone may get it\.\s+Nothing pushed yet: open its project in the editor and push\.$/);
    expect(history).not.toHaveBeenCalled();

    sets.mockResolvedValue({ ok: true, value: productsResponse({ products: [JO_PRODUCT, { ...nightOps, visibility: 'private' }] }) });
    await page.get('#product-visibility').trigger('click');
    await flushPromises();
    expect(page.get('[role="status"]').text()).toBe('Only its owner and admins may now get NIGHTOPS.');
    expect(page.get('#product-project-line').text()).toContain('private: its owner and admins only.');

    await page.get('#product-rename-toggle').trigger('click');
    expect(page.get('#product-rename-form').text()).toContain('Editors find a project by its key: a project file naming the old key no longer reaches it.');
    await page.get('#product-delete-toggle').trigger('click');
    expect(page.get('#product-delete-form').text()).toContain('editors can no longer pull or push it.');
    expect(page.text()).not.toMatch(/\bbind|\bbound\b/i);
  });

  it("changes the project's visibility and renames its key, following it to its new address", async () => {
    update.mockResolvedValueOnce({ ok: true, value: { ...JO_PRODUCT, visibility: 'public' } });
    const page = await mountProduct();
    await page.get('#product-visibility').trigger('click');
    await flushPromises();
    expect(update).toHaveBeenCalledWith(1, { visibility: 'public' });
    expect(page.get('[role="status"]').text()).toBe('Anyone may now get JO.');

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

  it('builds a release from a revision once its version reads right', async () => {
    build.mockResolvedValue({ ok: true, value: release(12, { status: 'building', version: '1.7.5.12' }) });
    const page = await mountProduct();
    await page.get('#product-build-toggle').trigger('click');
    expect((page.get('#product-build-revision').element as HTMLInputElement).value).toBe('2');
    await page.get('#product-build-version').setValue('1.7.5.x');
    submitForm(page, '#product-build-version');
    await flushPromises();
    expect(build).not.toHaveBeenCalled();
    expect(page.get('#product-build-form').text()).toContain('A version is one to four numbers between dots.');

    await page.get('#product-build-version').setValue('1.7.5.12');
    submitForm(page, '#product-build-version');
    await flushPromises();
    expect(build).toHaveBeenCalledWith('JO', { version: '1.7.5.12', revision: 2 });
    expect(page.get('[role="status"]').text()).toBe('1.7.5.12 is building: a draft to publish here once it is checked.');
  });

  it('publishes, retires and deletes a release, each behind a confirm', async () => {
    publish.mockResolvedValue({ ok: true, value: release(8) });
    retire.mockResolvedValue({ ok: false, error: { code: 'not_published', status: 409, message: 'That release is not published. Refresh the list.' } });
    removeRelease.mockResolvedValue({ ok: true, value: true });
    const page = await mountProduct();
    const rows = () => page.findAll('tr.product-release');

    await rows()[2].findAll('button').find((b) => b.text().startsWith('Publish'))!.trigger('click');
    await flushPromises();
    expect(confirm).toHaveBeenLastCalledWith(expect.stringContaining('Publish 1.7.5.8?'));
    expect(publish).toHaveBeenCalledWith(8);
    expect(page.get('[role="status"]').text()).toBe('1.7.5.8 is published.');

    await rows()[1].findAll('button').find((b) => b.text().startsWith('Retire'))!.trigger('click');
    await flushPromises();
    expect(retire).toHaveBeenCalledWith(9);
    expect(page.get('[role="alert"]').text()).toBe('That release is not published. Refresh the list.');

    confirm.mockResolvedValueOnce(false);
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
    const page = await mountProduct();
    await page.get('#product-update-servers').trigger('click');
    await flushPromises();
    expect(confirm).toHaveBeenCalledWith(expect.stringContaining('Move the 1 server of Joint Operations on an older release to 1.7.5.9?'));
    expect(updateAll).toHaveBeenCalledWith(1);
    expect(page.get('[role="status"]').text()).toBe('1 server is moving to 1.7.5.9.');
  });

  it('deletes the product behind its key typed, and says why one stays', async () => {
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
    expect(page.find('#product-project').exists()).toBe(false);
    expect(page.find('#product-servers').exists()).toBe(false);
    expect(page.find('#product-edit-toggle').exists()).toBe(false);
    expect(page.find('#product-delete-toggle').exists()).toBe(false);
    expect(page.find('#product-build-toggle').exists()).toBe(false);
    expect(page.text()).toContain('Published by the Publish workflow');
    expect(history).not.toHaveBeenCalled();
    const rows = page.findAll('tr.product-release');
    expect(rows.map((r) => r.find('th span').text())).toEqual(['v0.0.21', 'v0.0.21']);
    expect(rows[0].text()).toContain('Published');
    expect(rows[1].text()).toContain('Uploading');
    expect(buttonNamed(page, 'tbody', 'Retire').exists()).toBe(true);
  });

  it.each(['rename', 'build'] as const)('leaves a deleted product without warning about its %s draft', async (draft) => {
    router = createRouter({
      history: createMemoryHistory(),
      routes: [
        { path: '/admin/products/:ref', component: ProductSection },
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
    expect(page.text()).toContain('No product has the key or id NOPE.');
  });
});
