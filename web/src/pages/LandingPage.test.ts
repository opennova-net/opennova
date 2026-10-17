import { flushPromises, mount } from '@vue/test-utils';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import LandingPage from './LandingPage.vue';
import { fetchStats } from '../api/stats';
import { fetchTiers } from '../api/billing';
import { tiersResponse } from '../test/billingFixtures';
import { testRouter } from '../test/router';

vi.mock('../api/stats', () => ({ fetchStats: vi.fn() }));
vi.mock('../api/billing', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../api/billing')>()),
  fetchTiers: vi.fn(),
}));

const stats = vi.mocked(fetchStats);
const tiers = vi.mocked(fetchTiers);

async function mountPage() {
  const router = await testRouter('/');
  const wrapper = mount(LandingPage, { global: { plugins: [router] } });
  await flushPromises();
  return wrapper;
}

function hrefs(wrapper: Awaited<ReturnType<typeof mountPage>>) {
  return wrapper.findAll('a').map((a) => a.attributes('href'));
}

describe('LandingPage', () => {
  beforeEach(() => {
    vi.clearAllMocks();
    stats.mockResolvedValue({ games: 3, lobbies: 2, players: 9, updatedAtMs: 0 });
    tiers.mockResolvedValue({ ok: true, value: tiersResponse() });
  });

  it('leads with our own game, and still plays the Joint Operations you own', async () => {
    const wrapper = await mountPage();
    const text = wrapper.text();
    expect(wrapper.get('h1').text()).toBe('OpenNova');
    expect(text).toContain("A game on NovaLogic's engine, rebuilt in the open.");
    expect(text).toContain('Indigo Shore');
    expect(text).toContain('Online play in our own game is coming.');
    expect(text).toContain('OpenNova runs in place of Jointops.exe.');
    expect(text).toContain('It ships no retail files.');
    expect(text).toContain('Pre-1.0 and experimental');
    // Public copy rules: no em dash, no maturity claim, no license.
    expect(text).not.toContain('—');
    expect(text).not.toMatch(/production-ready/i);
    expect(text).not.toMatch(/\bMIT\b|licen[cs]e/i);
    // dfx2.exe is named only as an original exe the Switcher serves, never as one OpenNova replaces (yet).
    expect(text).not.toMatch(/in place of (Jointops\.exe and |Jointops\.exe or )?dfx2\.exe/);
  });

  it('says parity is the aim, not a promise', async () => {
    const wrapper = await mountPage();
    const text = wrapper.text();
    expect(text).toContain('The aim is parity with the game as it shipped');
    expect(text).toContain('OpenNova is pre-1.0, so expect differences');
    expect(hrefs(wrapper)).toContain('https://github.com/opennova-net/opennova/issues');
  });

  it('keeps the Switcher to the original exes: OpenNova reaches NovaWorld as it is', async () => {
    const text = (await mountPage()).text();
    expect(text).toContain('With OpenNova');
    expect(text).toContain('Nothing else to install.');
    expect(text).toContain('With the original Jointops.exe or dfx2.exe');
    expect(text).toContain('gs.novaworld.net');
    expect(text).toContain("OpenNova itself doesn't need it.");
  });

  it('says hosted servers run our engine, and how to run one yourself for free', async () => {
    const wrapper = await mountPage();
    const text = wrapper.text();
    expect(text).toContain("Hosted servers run OpenNova's own server, opennova-serve, not the original game.");
    expect(text).toContain("we can't guarantee that yet");
    expect(text).toContain('not the sale of any game');
    expect(text).toContain('Rather not pay? Run it yourself.');
    expect(text).toContain('--master-host nw.opennova.net');
    const links = hrefs(wrapper);
    expect(links).toContain('https://github.com/opennova-net/opennova/blob/master/apps/serve/README.md');
    expect(links).toContain('https://github.com/opennova-net/opennova/blob/master/DEPLOY.md');
  });

  it('shows the live counts', async () => {
    const wrapper = await mountPage();
    expect(wrapper.get('[role="status"]').text()).toBe('9 players across 2 lobbies right now.');
  });

  it('says when no one is hosting, and when the counts did not load', async () => {
    stats.mockResolvedValueOnce({ games: 3, lobbies: 0, players: 0, updatedAtMs: 0 });
    expect((await mountPage()).get('[role="status"]').text()).toBe('NovaWorld is online. No one is hosting right now.');

    stats.mockRejectedValueOnce(new Error('offline'));
    expect((await mountPage()).get('[role="status"]').text()).toBe('NovaWorld status unavailable.');
  });

  it('lists the tiers with their prices and the own machine add-on', async () => {
    const wrapper = await mountPage();
    const text = wrapper.text();
    expect(text).toContain('$5');
    expect(text).toContain('$10');
    expect(text).toContain('$15');
    expect(text).toContain('1 hosted server');
    expect(text).toContain('3 hosted servers');
    expect(text).toContain('Own machine add-on: $45 a month for each');
    expect(hrefs(wrapper)).toContain('/support');
  });

  it('says when tiers are not open, and when the prices did not load', async () => {
    tiers.mockResolvedValueOnce({ ok: true, value: tiersResponse({ enabled: false }) });
    let wrapper = await mountPage();
    expect(wrapper.text()).not.toContain('$5');
    expect(wrapper.text()).toContain('Support tiers are not open on NovaWorld yet.');

    tiers.mockResolvedValueOnce({ ok: false, error: { code: 'network', status: null, message: 'offline' } });
    wrapper = await mountPage();
    expect(wrapper.text()).toContain('The prices did not load.');
    expect(wrapper.text()).not.toContain('Own machine add-on');
  });

  it('offers every release download and the account and lobby steps', async () => {
    const wrapper = await mountPage();
    const text = wrapper.text();
    for (const file of [
      'opennova-game-windows',
      'opennova-editor-windows',
      'opennova-switcher-windows',
      'opennova-blender-addon-windows',
      'opennova-apps-windows',
    ]) {
      expect(text).toContain(file);
    }
    const links = hrefs(wrapper);
    expect(links).toContain('https://github.com/opennova-net/opennova/releases/latest');
    expect(links).toContain('/register');
    expect(links).toContain('/lobby');
    expect(links).toContain('#downloads');
    expect(links).toContain('#editor');
  });

  it('leaves the time-lapse to the visitor: controls, no autoplay', async () => {
    const video = (await mountPage()).get('video');
    expect(video.attributes()).toHaveProperty('controls');
    expect(video.attributes()).not.toHaveProperty('autoplay');
    expect(video.attributes('preload')).toBe('none');
  });
});
