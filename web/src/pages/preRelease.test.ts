import { flushPromises, mount } from '@vue/test-utils';
import { describe, expect, it, vi } from 'vitest';
import type { Component } from 'vue';
import DownloadsPage from './DownloadsPage.vue';
import HostingPage from './HostingPage.vue';
import LandingPage from './LandingPage.vue';
import PlayPage from './PlayPage.vue';
import SiteFooter from '../components/SiteFooter.vue';
import { tiersResponse } from '../test/billingFixtures';
import { testRouter } from '../test/router';

// "Pre-1.0 and experimental" is said in two places only: the footer, on every
// page, and the downloads.
vi.mock('../api/stats', () => ({ fetchStats: vi.fn().mockResolvedValue({ games: 1, lobbies: 1, players: 1, updatedAtMs: 0 }) }));
vi.mock('../api/billing', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../api/billing')>()),
  fetchTiers: vi.fn(async () => ({ ok: true, value: tiersResponse() })),
}));
vi.mock('../api/downloads', () => ({ downloadsBase: vi.fn(() => null), fetchDownloadsManifest: vi.fn() }));

async function textOf(component: Component, path: string): Promise<string> {
  const router = await testRouter(path);
  const wrapper = mount(component, { global: { plugins: [router] } });
  await flushPromises();
  const text = wrapper.text();
  wrapper.unmount();
  return text;
}

const PRE_RELEASE = /pre-1\.0|experimental/i;

describe('the pre-release notice', () => {
  it.each([
    ['the home page', LandingPage, '/'],
    ['Play', PlayPage, '/play'],
    ['Hosting', HostingPage, '/hosting'],
  ])('is not on %s', async (_name, component, path) => {
    expect(await textOf(component, path)).not.toMatch(PRE_RELEASE);
  });

  it.each([
    ['the footer', SiteFooter, '/'],
    ['Downloads', DownloadsPage, '/downloads'],
  ])('is in %s', async (_name, component, path) => {
    expect(await textOf(component, path)).toMatch(/pre-1\.0 and experimental/i);
  });
});
