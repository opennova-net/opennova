import { flushPromises, mount, type VueWrapper } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import type { Router } from 'vue-router';
import AssetsPage from './AssetsPage.vue';
import { fetchAssets, prepareAssetDownload } from '../api/assets';
import { sharedAsset } from '../test/assetFixtures';
import { testRouter } from '../test/router';

vi.mock('../api/assets', async (original) => ({
  ...(await original<typeof import('../api/assets')>()),
  fetchAssets: vi.fn(),
  prepareAssetDownload: vi.fn(),
}));
const list = vi.mocked(fetchAssets);
const prepare = vi.mocked(prepareAssetDownload);

describe('AssetsPage', () => {
  let wrapper: VueWrapper | null = null;
  let router: Router;

  async function mountPage(path = '/assets') {
    router = await testRouter(path);
    wrapper = mount(AssetsPage, { global: { plugins: [router] } });
    await flushPromises();
    return wrapper;
  }

  beforeEach(() => {
    vi.resetAllMocks();
    list.mockResolvedValue({ ok: true, value: { assets: [sharedAsset()], next_before: null } });
    prepare.mockResolvedValue({ ok: true, value: { state: 'preparing' } });
  });

  afterEach(() => {
    wrapper?.unmount();
    wrapper = null;
    vi.restoreAllMocks();
    vi.useRealTimers();
  });

  it('shows native assets with their project, published version and bundle contents', async () => {
    const page = await mountPage();
    expect(page.get('h1').text()).toBe('Asset library');
    expect(list).toHaveBeenCalledWith({}, expect.any(AbortSignal));
    const row = page.get('.asset-row');
    expect(row.text()).toContain('crate.3di');
    expect(row.text()).toContain('Models (.3di)');
    expect(row.text()).toContain('by biggy');
    expect(row.text()).toContain('0.2.0');
    expect(row.text()).toContain('3 files · 1.2 MB');
    expect(row.get('a').attributes('href')).toBe('/assets?project=NIGHTOPS');
    expect(row.get('button').attributes('aria-label')).toBe('Download crate.3di');
  });

  it('keeps filters in the URL, resets pagination on search and preserves filters on Next', async () => {
    list.mockResolvedValue({ ok: true, value: { assets: [sharedAsset()], next_before: 12 } });
    const page = await mountPage('/assets?q=crate&kind=model&project=NIGHTOPS&before=40');
    expect(list).toHaveBeenLastCalledWith({ q: 'crate', kind: 'model', project: 'NIGHTOPS', before: 40 }, expect.any(AbortSignal));
    expect((page.get('#assets-search').element as HTMLInputElement).value).toBe('crate');
    const next = page.findAll('a').find((link) => link.text() === 'Next')!;
    expect(next.attributes('href')).toBe('/assets?q=crate&kind=model&project=NIGHTOPS&before=12');
    expect(page.findAll('a').find((link) => link.text() === 'First page')!.attributes('href')).toBe('/assets?q=crate&kind=model&project=NIGHTOPS');
    await page.get('#assets-search').setValue(' island ');
    await page.get('#assets-kind').setValue('terrain');
    await page.get('#assets-project').setValue('ACIDGAMBIT');
    await page.get('form').trigger('submit');
    await flushPromises();
    expect(router.currentRoute.value.query).toEqual({ q: 'island', kind: 'terrain', project: 'ACIDGAMBIT' });
    expect(list).toHaveBeenLastCalledWith({ q: 'island', kind: 'terrain', project: 'ACIDGAMBIT' }, expect.any(AbortSignal));
  });

  it('does not let an old request replace a newer search', async () => {
    let finish!: (value: Awaited<ReturnType<typeof fetchAssets>>) => void;
    list.mockImplementationOnce(() => new Promise((resolve) => { finish = resolve; }));
    const page = await mountPage();
    const firstSignal = list.mock.calls[0][1]!;
    await router.push('/assets?q=island');
    await flushPromises();
    expect(firstSignal.aborted).toBe(true);
    finish({ ok: true, value: { assets: [], next_before: null } });
    await flushPromises();
    expect(page.find('.asset-row').exists()).toBe(true);
  });

  it('shows empty, filtered-empty and failed reads with a refresh action', async () => {
    list.mockResolvedValue({ ok: true, value: { assets: [], next_before: null } });
    const page = await mountPage();
    expect(page.text()).toContain('No shared assets yet.');
    await router.push('/assets?q=missing');
    await flushPromises();
    expect(page.text()).toContain('No assets match these filters.');
    list.mockResolvedValue({ ok: false, error: 'The asset library could not be read. Try again.' });
    await page.findAll('button').find((button) => button.text() === 'Refresh')!.trigger('click');
    await flushPromises();
    expect(page.get('[role="alert"]').text()).toContain('could not be read');
  });

  it('prepares a bundle then starts its named download after polling', async () => {
    vi.useFakeTimers();
    const click = vi.spyOn(HTMLAnchorElement.prototype, 'click').mockImplementation(function (this: HTMLAnchorElement) {
      expect(this.href).toBe('https://bucket.example/crate.zip?signature=abc');
      expect(this.download).toBe('crate.zip');
    });
    prepare.mockResolvedValueOnce({ ok: true, value: { state: 'preparing' } })
      .mockResolvedValueOnce({ ok: true, value: { state: 'ready', url: 'https://bucket.example/crate.zip?signature=abc', filename: 'crate.zip' } });
    const page = await mountPage();
    await page.get('.asset-row button').trigger('click');
    await flushPromises();
    expect(page.get('.asset-row button').text()).toBe('Preparing…');
    expect(page.get('.asset-row button').attributes('disabled')).toBeDefined();
    await vi.advanceTimersByTimeAsync(1999);
    expect(prepare).toHaveBeenCalledTimes(1);
    await vi.advanceTimersByTimeAsync(1);
    await flushPromises();
    expect(prepare).toHaveBeenCalledTimes(2);
    expect(prepare).toHaveBeenNthCalledWith(1, 12, expect.any(AbortSignal), true);
    expect(prepare).toHaveBeenNthCalledWith(2, 12, expect.any(AbortSignal), false);
    expect(click).toHaveBeenCalledOnce();
    expect(page.get('.asset-row button').text()).toBe('Download');
    expect(page.get('.asset-row button').attributes('disabled')).toBeUndefined();
  });

  it('allows retry after a download failure', async () => {
    prepare.mockResolvedValueOnce({ ok: false, error: 'The download could not be prepared. Try again.' });
    const page = await mountPage();
    await page.get('.asset-row button').trigger('click');
    await flushPromises();
    expect(page.get('[role="alert"]').text()).toContain('crate.3di: The download could not be prepared.');
    expect(page.get('.asset-row button').attributes('disabled')).toBeUndefined();
    await page.get('.asset-row button').trigger('click');
    await flushPromises();
    expect(prepare).toHaveBeenNthCalledWith(2, 12, expect.any(AbortSignal), true);
  });

  it('cancels pending bundle work and list reads when the page closes', async () => {
    vi.useFakeTimers();
    const page = await mountPage();
    const listSignal = list.mock.calls[0][1]!;
    await page.get('.asset-row button').trigger('click');
    await flushPromises();
    const downloadSignal = prepare.mock.calls[0][1]!;
    page.unmount();
    wrapper = null;
    expect(listSignal.aborted).toBe(true);
    expect(downloadSignal.aborted).toBe(true);
    await vi.advanceTimersByTimeAsync(5000);
    expect(prepare).toHaveBeenCalledOnce();
  });
});
