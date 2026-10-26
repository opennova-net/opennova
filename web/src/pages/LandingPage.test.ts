import { flushPromises, mount } from '@vue/test-utils';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import LandingPage from './LandingPage.vue';
import { fetchStats } from '../api/stats';
import { testRouter } from '../test/router';
vi.mock('../api/stats', () => ({ fetchStats: vi.fn() }));
const stats = vi.mocked(fetchStats);
async function open() {
  const router = await testRouter('/');
  const wrapper = mount(LandingPage, { global: { plugins: [router] } });
  await flushPromises();
  return wrapper;
}
describe('LandingPage', () => {
  beforeEach(() => { stats.mockResolvedValue({ games: 3, lobbies: 2, players: 9, updatedAtMs: 0 }); });
  it('introduces play, hosting and creation while keeping legacy destinations useful', async () => {
    const page = await open();
    expect(page.get('h1').text()).toBe('OpenNova');
    const links = page.findAll('a').map((a) => a.attributes('href'));
    for (const href of ['/play', '/play?game=jointops', '/lobby', '/support', '/downloads', '/downloads#tools']) expect(links).toContain(href);
    for (const id of ['game', 'retail', 'online', 'editor', 'downloads', 'run-it-yourself']) expect(page.find(`#${id}`).exists()).toBe(true);
    expect(page.text()).toContain('OP: Acid Gambit');
    expect(page.text()).toContain('OP: Nifty Package');
    expect(page.text()).toContain('parity with the game as it shipped');
    expect(page.text()).toContain('not the sale of any game');
    expect(page.text()).toContain('Pre-1.0 and experimental');
    expect(page.text()).not.toMatch(/production-ready|—/);
    expect(page.find('a[href*=".zip"]').exists()).toBe(false);
  });
  it('describes counts as activity rather than promising service health', async () => {
    expect((await open()).get('[role="status"]').text()).toBe('9 players across 2 servers on our NovaWorld.');
    stats.mockResolvedValueOnce({ games: 3, lobbies: 0, players: 0, updatedAtMs: 0 });
    expect((await open()).get('[role="status"]').text()).toBe('No players or servers listed on our NovaWorld right now.');
    stats.mockRejectedValueOnce(new Error('offline'));
    expect((await open()).get('[role="status"]').text()).toBe('Player activity is unavailable right now.');
  });
  it('uses real project media and leaves playback to the visitor', async () => {
    const page = await open();
    expect(page.get('img').attributes('src')).toBe('/media/hero.webp');
    const video = page.get('video');
    expect(video.attributes()).toHaveProperty('controls');
    expect(video.attributes()).not.toHaveProperty('autoplay');
    expect(video.attributes('preload')).toBe('none');
  });
});
