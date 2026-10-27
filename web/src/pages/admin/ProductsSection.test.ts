import { flushPromises, mount, type VueWrapper } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import type { Router } from 'vue-router';
import ProductsSection from './ProductsSection.vue';
import { createProduct, fetchProducts, fetchReleases } from '../../api/products';
import { testRouter } from '../../test/router';
import { JO_PRODUCT, product, productsResponse, release, runtimeRelease } from '../../test/productFixtures';

vi.mock('../../api/products', () => ({
  createProduct: vi.fn(),
  fetchProducts: vi.fn(),
  fetchReleases: vi.fn(),
}));

const sets = vi.mocked(fetchProducts);
const list = vi.mocked(fetchReleases);
const create = vi.mocked(createProduct);

function submitForm(page: VueWrapper, selector: string) {
  return page.get(selector).element.closest('form')!.dispatchEvent(new Event('submit'));
}

describe('ProductsSection', () => {
  let wrapper: VueWrapper | null = null;
  let router: Router;

  async function mountSection() {
    router = await testRouter('/admin/products');
    wrapper = mount(ProductsSection, { global: { plugins: [router] } });
    await flushPromises();
    return wrapper;
  }

  beforeEach(() => {
    vi.clearAllMocks();
    sets.mockResolvedValue({ ok: true, value: productsResponse() });
    list.mockResolvedValue({ ok: true, value: [release(10, { status: 'draft' }), release(9), runtimeRelease(7)] });
  });

  afterEach(() => {
    wrapper?.unmount();
    wrapper = null;
  });

  it('lists each product with its project, its published release and its servers, linking to its page', async () => {
    const page = await mountSection();
    const rows = page.findAll('tr.products-row');
    expect(rows.map((r) => r.get('th a').text())).toEqual(['Joint Operations', 'Escalation', 'Delta Force Xtreme 2', 'OpenNova runtime']);
    expect(rows.map((r) => r.get('th a').attributes('href'))).toEqual(['/admin/products/JO', '/admin/products/JOX01', '/admin/products/DFX2', '/admin/products/ONRUNTIME']);
    expect(rows[0].text()).toContain('Game Joint Operations, lobby Joint Operations');
    expect(rows[0].text()).toContain('Private, r2');
    expect(rows[0].text()).toContain('1.7.5.9');
    expect(rows[0].text()).toContain('3 live');
    expect(rows[0].text()).toContain('1 on an older release');
    expect(rows[1].text()).toContain('Expansion jox01 of Joint Operations');
    expect(rows[1].text()).toContain('nothing pushed');
    expect(rows[2].text()).toContain('None');
    expect(rows[3].text()).toContain('From CI');
    expect(rows[3].text()).toContain('v0.0.21');
    expect(rows.map((r) => r.get('.products-owner').text())).toEqual(['OpenNova', 'OpenNova', 'OpenNova', 'OpenNova']);
    // No em dash in the page's copy, no upload anywhere, and no editor "binds".
    expect(page.text()).not.toContain('—');
    expect(page.find('input[type="file"]').exists()).toBe(false);
    await page.get('#products-new-toggle').trigger('click');
    expect(page.text()).not.toMatch(/\bbind/i);
  });

  it("names a player's product's owner, linking to the account", async () => {
    sets.mockResolvedValue({
      ok: true,
      value: productsResponse({ products: [JO_PRODUCT, product(21, { display_name: 'Night Ops', kind: 'expansion', parent_id: 1, expansion_code: 'nightops', key: 'NIGHTOPS', owner: 'biggy', owner_id: 7 })] }),
    });
    const page = await mountSection();
    const owner = page.findAll('tr.products-row')[1].get('.products-owner a');
    expect(owner.text()).toBe('biggy');
    expect(owner.attributes('href')).toBe('/admin/users/7');
  });

  it('says why the products could not be read', async () => {
    sets.mockResolvedValue({ ok: false, error: { code: 'forbidden', status: 403, message: 'This account is not an admin.' } });
    const page = await mountSection();
    expect(page.get('[role="alert"]').text()).toBe('This account is not an admin.');
  });

  it('checks the new game form, sends the game with its project, and opens its page', async () => {
    create.mockResolvedValue({ ok: true, value: product(5, { display_name: 'Escalation 2', kind: 'expansion', key: 'JOX02' }) });
    const page = await mountSection();
    await page.get('#products-new-toggle').trigger('click');
    expect((page.get('#products-new-title').element as HTMLSelectElement).value).toBe('jo');
    expect((page.get('#products-new-lobby').element as HTMLSelectElement).value).toBe('jop_2_consumer');

    // Nothing named, no key: refused in the page.
    submitForm(page, '#products-new-display_name');
    await flushPromises();
    expect(create).not.toHaveBeenCalled();
    expect(page.get('#products-new-display_name-error').text()).toBe('Enter a name.');
    expect(page.text()).toContain('A key is 1 to 16 of A-Z, 0-9 and _.');

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

  it("shows the service's refusal of a new game", async () => {
    create.mockResolvedValue({ ok: false, error: { code: 'duplicate_key', status: 409, message: 'Another product already has that key.' } });
    const page = await mountSection();
    await page.get('#products-new-toggle').trigger('click');
    await page.get('#products-new-display_name').setValue('Joint Operations');
    await page.get('#products-new-key').setValue('JO');
    submitForm(page, '#products-new-display_name');
    await flushPromises();
    expect(create).toHaveBeenCalledWith(expect.objectContaining({ kind: 'game', key: 'JO', title: 'jo', lobby: 'jop_2_consumer' }));
    expect(page.get('[role="alert"]').text()).toBe('Another product already has that key.');
    expect(page.find('#products-new-form').exists()).toBe(true);
  });
});
