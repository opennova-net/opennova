import { AxiosError, type AxiosResponse } from 'axios';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import { apiClient } from './client';
import { ASSETS_PAGE_SIZE, fetchAssets, prepareAssetDownload } from './assets';
import { sharedAsset } from '../test/assetFixtures';

vi.mock('./client', () => ({ apiClient: { get: vi.fn(), post: vi.fn() } }));
const get = vi.mocked(apiClient.get);
const post = vi.mocked(apiClient.post);
const response = (data: unknown, status = 200) => ({ data, status }) as AxiosResponse;

describe('the public asset API', () => {
  beforeEach(() => vi.resetAllMocks());

  it('reads a cursor page with filename, kind and project filters', async () => {
    const page = { assets: [sharedAsset()], next_before: 12 };
    get.mockResolvedValue(response(page));
    const controller = new AbortController();
    const filters = { q: 'crate', kind: 'model' as const, project: 'NIGHTOPS', before: 40 };
    expect(await fetchAssets(filters, controller.signal)).toEqual({ ok: true, value: page });
    expect(get).toHaveBeenCalledWith('/assets', { params: { ...filters, limit: ASSETS_PAGE_SIZE }, signal: controller.signal });
    get.mockResolvedValue(response({ assets: [], next_before: null }));
    expect(await fetchAssets({})).toEqual({ ok: true, value: { assets: [], next_before: null } });
  });

  it('refuses malformed pages and unknown asset kinds', async () => {
    for (const page of [
      { assets: [sharedAsset(12, { kind: 'other' as never })], next_before: null },
      { assets: [sharedAsset(12, { file_count: 0 })], next_before: null },
      { assets: [sharedAsset()], next_before: '12' },
      { assets: [{ ...sharedAsset(), project: null }], next_before: null },
    ]) {
      get.mockResolvedValue(response(page));
      expect((await fetchAssets({})).ok).toBe(false);
    }
  });

  it('requests bundles without requiring a source-project download', async () => {
    const controller = new AbortController();
    post.mockResolvedValue(response({ state: 'preparing' }, 202));
    expect(await prepareAssetDownload(12, controller.signal)).toEqual({ ok: true, value: { state: 'preparing' } });
    expect(post).toHaveBeenCalledWith('/assets/12/download', {}, { signal: controller.signal });
    expect(await prepareAssetDownload(12, controller.signal, true)).toEqual({ ok: true, value: { state: 'preparing' } });
    expect(post).toHaveBeenLastCalledWith('/assets/12/download', { retry: true }, { signal: controller.signal });
    for (const url of ['https://bucket.s3.us-east-1.amazonaws.com/bundle?signature=abc', '/api/assets/12/bundle']) {
      const ready = { state: 'ready', url, filename: 'crate.zip' };
      post.mockResolvedValue(response(ready));
      expect(await prepareAssetDownload(12)).toEqual({ ok: true, value: ready });
    }
  });

  it('refuses active download URLs and inconsistent preparation replies', async () => {
    for (const url of ['javascript:alert(1)', 'data:text/html,bad', '//another.example/bundle', 'https://user:password@example.com/a.zip']) {
      post.mockResolvedValue(response({ state: 'ready', url, filename: 'crate.zip' }));
      expect((await prepareAssetDownload(12)).ok).toBe(false);
    }
    post.mockResolvedValue(response({ state: 'preparing' }, 200));
    expect((await prepareAssetDownload(12)).ok).toBe(false);
  });

  it('explains a withdrawn asset, rate limit and unavailable library', async () => {
    const refused = (status: number) => new AxiosError('refused', undefined, undefined, undefined, response({}, status));
    post.mockRejectedValue(refused(404));
    expect(await prepareAssetDownload(12)).toEqual({ ok: false, error: 'This asset is no longer shared. Refresh the library.' });
    post.mockRejectedValue(refused(429));
    expect(await prepareAssetDownload(12)).toEqual({ ok: false, error: 'Too many requests. Wait a moment and try again.' });
    get.mockRejectedValue(new Error('offline'));
    expect(await fetchAssets({})).toEqual({ ok: false, error: 'The asset library could not be read. Try again.' });
  });
});
