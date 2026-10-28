import * as actionReview from '../../../composables/useConfirm';
import { flushPromises, mount, type VueWrapper } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi, type MockInstance } from 'vitest';
import type { Router } from 'vue-router';
import ProductsPage from './ProductsPage.vue';
import { createProduct, fetchProducts, fetchReleases, publishRelease, rejectRelease } from '../../../api/products';
import { testRouter } from '../../../test/router';
import { JO_PRODUCT, product, productsResponse, release, runtimeRelease } from '../../../test/productFixtures';

vi.mock('../../../api/products', () => ({
  createProduct: vi.fn(),
  fetchProducts: vi.fn(),
  fetchReleases: vi.fn(),
  publishRelease: vi.fn(),
  rejectRelease: vi.fn(),
}));

const sets = vi.mocked(fetchProducts);
const list = vi.mocked(fetchReleases);
const create = vi.mocked(createProduct);
const publish = vi.mocked(publishRelease);
const reject = vi.mocked(rejectRelease);

const NIGHT_OPS = product(21, { display_name: 'Night Ops', kind: 'expansion', parent_id: 1, expansion_code: 'nightops', title: '', lobby: '', key: 'NIGHTOPS', owner: 'biggy', owner_id: 7 });
const waiting = (id: number, productId: number) =>
  release(id, { product_id: productId, status: 'awaiting_approval', version: `0.${id}.0`, created_by: 'user biggy', sections: [] });

function submitForm(page: VueWrapper, selector: string) {
  return page.get(selector).element.closest('form')!.dispatchEvent(new Event('submit'));
}

describe('ProductsPage', () => {
  let wrapper: VueWrapper | null = null;
  let router: Router;
  let confirm: MockInstance<typeof actionReview.confirmAction>;

  async function mountPage() {
    router = await testRouter('/admin/products');
    wrapper = mount(ProductsPage, { global: { plugins: [router] } });
    await flushPromises();
    return wrapper;
  }

  beforeEach(() => {
    vi.clearAllMocks();
    confirm = vi.spyOn(actionReview, 'confirmAction').mockResolvedValue(true);
    sets.mockResolvedValue({ ok: true, value: productsResponse() });
    list.mockResolvedValue({ ok: true, value: [release(10, { status: 'draft' }), release(9), runtimeRelease(7)] });
  });

  afterEach(() => {
    wrapper?.unmount();
    wrapper = null;
    confirm.mockRestore();
  });

  it('is a title, a New game action and one table of products, each linking its page, owner and servers', async () => {
    const page = await mountPage();
    expect(page.get('h1').text()).toBe('Products');
    expect(page.get('#products-new-toggle').text()).toBe('New game');
    expect(page.find('#products-awaiting').exists()).toBe(false);

    const table = page.findAll('table').at(-1)!;
    expect(table.findAll('thead th').map((th) => th.text())).toEqual(['Product', 'Kind', 'Owner', 'Published', 'Servers']);
    const rows = page.findAll('tr.products-row');
    expect(rows.map((r) => r.get('th a').text())).toEqual(['Joint Operations', 'Escalation', 'Delta Force Xtreme 2', 'OpenNova runtime']);
    expect(rows.map((r) => r.get('th a').attributes('href'))).toEqual(['/admin/products/JO', '/admin/products/JOX01', '/admin/products/DFX2', '/admin/products/ONRUNTIME']);
    expect(rows[0].get('.products-kind').text()).toBe('Game Joint Operations, lobby Joint Operations');
    expect(rows[1].get('.products-kind').text()).toBe('Expansion jox01 of Joint Operations');
    expect(rows[3].get('.products-kind').text()).toBe('App, built by CI');
    expect(rows[0].text()).toContain('1.7.5.9');
    expect(rows[2].text()).toContain('None');
    expect(rows[3].text()).toContain('v0.0.21');
    // Servers: the live count to the servers filtered to the product, and the outdated ones.
    const servers = rows[0].get('.products-servers a');
    expect([servers.text(), servers.attributes('href')]).toEqual(['3 live', '/admin/servers?product=1']);
    expect(rows[0].get('.products-servers').text()).toContain('1 outdated');
    expect(rows[3].get('.products-servers').text()).toBe('');
    expect(rows.map((r) => r.get('.products-owner').text())).toEqual(['OpenNova', 'OpenNova', 'OpenNova', 'OpenNova']);
    expect(page.text()).not.toContain('—');
    expect(page.find('input[type="file"]').exists()).toBe(false);
  });

  it("links a player's product's owner to their page", async () => {
    sets.mockResolvedValue({ ok: true, value: productsResponse({ products: [JO_PRODUCT, NIGHT_OPS] }) });
    const page = await mountPage();
    const owner = page.findAll('tr.products-row')[1].get('.products-owner a');
    expect([owner.text(), owner.attributes('href')]).toEqual(['biggy', '/admin/players/7']);
  });

  it('says why the products could not be read', async () => {
    sets.mockResolvedValue({ ok: false, error: { code: 'forbidden', status: 403, message: 'This account is not an admin.' } });
    const page = await mountPage();
    expect(page.get('[role="alert"]').text()).toBe('This account is not an admin.');
  });

  it('lists the releases awaiting approval first, approving one after a confirm', async () => {
    const rangers = product(22, { display_name: 'Rangers', key: 'RANGERS', owner: 'biggy', owner_id: 7 });
    sets.mockResolvedValue({ ok: true, value: productsResponse({ products: [JO_PRODUCT, NIGHT_OPS, rangers] }) });
    list.mockResolvedValue({ ok: true, value: [waiting(5, 21), waiting(4, 22), release(9)] });
    publish.mockResolvedValue({ ok: true, value: { ...waiting(5, 21), status: 'published' } });
    const page = await mountPage();

    const section = page.get('#products-awaiting').element.closest('section')!;
    expect(Array.from(section.querySelectorAll('thead th')).map((th) => th.textContent!.trim())).toEqual(['Product', 'Version', 'Owner', 'Submitted', 'Actions']);
    const rows = page.findAll('tr.products-awaiting-row');
    expect(rows).toHaveLength(2);
    expect(rows.map((r) => r.get('th a').attributes('href'))).toEqual(['/admin/products/NIGHTOPS', '/admin/products/RANGERS']);
    expect(rows[0].text()).toContain('0.5.0');
    expect(rows[0].findAll('a').map((a) => a.attributes('href'))).toContain('/admin/players/7');

    confirm.mockResolvedValueOnce(false);
    await rows[0].findAll('button').find((b) => b.text().startsWith('Approve'))!.trigger('click');
    await flushPromises();
    expect(publish).not.toHaveBeenCalled();

    await rows[0].findAll('button').find((b) => b.text().startsWith('Approve'))!.trigger('click');
    await flushPromises();
    expect(confirm).toHaveBeenLastCalledWith('Approve 0.5.0? Players get it and new servers run it.');
    expect(publish).toHaveBeenCalledWith(5);
    expect(page.get('[role="status"]').text()).toBe('0.5.0 is approved and published.');
  });

  it('sends a release back to its owner with a note', async () => {
    sets.mockResolvedValue({ ok: true, value: productsResponse({ products: [JO_PRODUCT, NIGHT_OPS] }) });
    list.mockResolvedValue({ ok: true, value: [waiting(5, 21)] });
    reject.mockResolvedValue({ ok: true, value: { ...waiting(5, 21), status: 'draft', review_note: 'Fix the briefing.' } });
    const page = await mountPage();
    expect(page.find('#products-reject-form').exists()).toBe(false);
    await page.get('tr.products-awaiting-row').findAll('button').find((b) => b.text().startsWith('Reject'))!.trigger('click');
    expect(page.get('#products-reject-form').text()).toContain('Note to biggy (optional)');
    await page.get('#products-reject-note').setValue('  Fix the briefing. ');
    submitForm(page, '#products-reject-note');
    await flushPromises();
    expect(reject).toHaveBeenCalledWith(5, 'Fix the briefing.');
    expect(page.get('[role="status"]').text()).toBe('0.5.0 is back with its owner as a draft.');
    expect(page.find('#products-reject-form').exists()).toBe(false);
  });

  it('checks the new game form, sends the game with its project, and opens its page', async () => {
    create.mockResolvedValue({ ok: true, value: product(5, { display_name: 'Escalation 2', kind: 'expansion', key: 'JOX02' }) });
    const page = await mountPage();
    expect(page.find('#products-new-form').exists()).toBe(false);
    await page.get('#products-new-toggle').trigger('click');
    expect(page.get('#products-new-toggle').attributes('aria-expanded')).toBe('true');
    expect((page.get('#products-new-title').element as HTMLSelectElement).value).toBe('jo');
    expect((page.get('#products-new-lobby').element as HTMLSelectElement).value).toBe('jop_2_consumer');

    // Nothing named, no key: refused in the page.
    submitForm(page, '#products-new-display_name');
    await flushPromises();
    expect(create).not.toHaveBeenCalled();
    expect(page.get('#products-new-display_name-error').text()).toBe('Enter a name.');
    expect(page.get('#products-new-key-error').text()).toBe('A key is 1 to 16 of A-Z, 0-9 and _.');

    // An expansion: its game among the games, its code's shape, the updater pair.
    await page.get('#products-new-kind').setValue('expansion');
    expect(page.findAll('#products-new-parent_id option').map((o) => o.text())).toEqual(['Joint Operations', 'Delta Force Xtreme 2']);
    await page.get('#products-new-display_name').setValue(' Escalation 2 ');
    await page.get('#products-new-key').setValue('jox02');
    await page.get('#products-new-expansion_code').setValue('JOX-02');
    await page.get('#products-new-updater_login').setValue('JOEus2');
    submitForm(page, '#products-new-display_name');
    await flushPromises();
    expect(create).not.toHaveBeenCalled();
    expect(page.get('#products-new-expansion_code-error').text()).toContain('1 to 8 lowercase letters and digits');
    expect(page.get('#products-new-command_file-error').text()).toBe('An updater login needs its command file.');

    await page.get('#products-new-expansion_code').setValue('jox02');
    await page.get('#products-new-command_file').setValue('joe2ptr.cmd');
    await page.get('#products-new-visibility').setValue('public');
    submitForm(page, '#products-new-display_name');
    await flushPromises();
    expect(create).toHaveBeenCalledWith({
      display_name: 'Escalation 2',
      kind: 'expansion',
      key: 'JOX02',
      visibility: 'public',
      retail: false,
      updater_login: 'JOEus2',
      command_file: 'JOE2PTR.CMD',
      parent_id: 1,
      expansion_code: 'jox02',
    });
    expect(router.currentRoute.value.path).toBe('/admin/products/JOX02');
  });

  it("shows the service's refusal of a new game and keeps the form", async () => {
    create.mockResolvedValue({ ok: false, error: { code: 'duplicate_key', status: 409, message: 'Another product already has that key.' } });
    const page = await mountPage();
    await page.get('#products-new-toggle').trigger('click');
    await page.get('#products-new-display_name').setValue('Joint Operations');
    await page.get('#products-new-key').setValue('JO');
    submitForm(page, '#products-new-display_name');
    await flushPromises();
    expect(create).toHaveBeenCalledWith(expect.objectContaining({ kind: 'game', key: 'JO', title: 'jo', lobby: 'jop_2_consumer' }));
    expect(page.get('[role="alert"]').text()).toBe('Another product already has that key.');
    expect(page.find('#products-new-form').exists()).toBe(true);
    // The toggle closes it again.
    await page.get('#products-new-toggle').trigger('click');
    expect(page.find('#products-new-form').exists()).toBe(false);
  });

  it('limits new game lobbies to its selected title', async () => {
    const page = await mountPage();
    await page.get('#products-new-toggle').trigger('click');
    expect(page.findAll('#products-new-lobby option').map((option) => option.attributes('value'))).toEqual(['jop_2_consumer', 'acidgambit_0_consumer']);
    await page.get('#products-new-title').setValue('dfx2');
    expect(page.findAll('#products-new-lobby option').map((option) => option.attributes('value'))).toEqual(['dfx2_consumer']);
    expect((page.get('#products-new-lobby').element as HTMLSelectElement).value).toBe('dfx2_consumer');
  });
});
