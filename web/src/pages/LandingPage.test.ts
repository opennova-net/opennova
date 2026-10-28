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
  beforeEach(() => {
    stats.mockResolvedValue({ games: 3, lobbies: 2, players: 9, updatedAtMs: 0 });
  });

  it('leads with Play and Download, then the three games and the editor', async () => {
    const page = await open();
    expect(page.get('h1').text()).toBe('OpenNova');
    const hero = page.get('section[aria-labelledby="hero-title"]');
    expect(hero.findAll('a.btn-primary, a.btn-secondary').map((a) => [a.text(), a.attributes('href')])).toEqual([
      ['Play', '/play'],
      ['Download', '/downloads'],
    ]);
    const cards = page.findAll('#games-title + ul > li');
    expect(cards.map((c) => c.get('h3').text())).toEqual(['OP: Acid Gambit', 'Joint Operations', 'Delta Force Xtreme 2']);
    expect(cards.map((c) => c.get('a').attributes('href'))).toEqual(['/play', '/play?game=jointops', '/play?game=dfx2']);
    // One sentence a card.
    for (const card of cards) expect(card.findAll('p')).toHaveLength(1);
    expect(page.find('#editor a[href="/downloads#apps"]').exists()).toBe(true);
    // No downloads strip: the nav has Download.
    expect(page.find('#downloads').exists()).toBe(false);
    expect(page.find('a[href*=".zip"]').exists()).toBe(false);
    expect(page.text()).not.toMatch(/production-ready|—/);
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
