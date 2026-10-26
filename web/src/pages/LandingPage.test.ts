import { flushPromises, mount } from '@vue/test-utils';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import LandingPage from './LandingPage.vue';
import { fetchStats } from '../api/stats';
import { fetchTiers } from '../api/billing';
import { downloadsBase, fetchDownloadsManifest } from '../api/downloads';
import { tiersResponse } from '../test/billingFixtures';
import { testRouter } from '../test/router';

vi.mock('../api/stats', () => ({ fetchStats: vi.fn() }));
vi.mock('../api/downloads', () => ({ downloadsBase: vi.fn(), fetchDownloadsManifest: vi.fn() }));
vi.mock('../api/billing', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../api/billing')>()),
  fetchTiers: vi.fn(),
}));

const stats = vi.mocked(fetchStats);
const tiers = vi.mocked(fetchTiers);
const host = vi.mocked(downloadsBase);
const manifest = vi.mocked(fetchDownloadsManifest);

const BUILDS = [
  'opennova-game-windows.zip',
  'jointops-game-windows.zip',
  'dfx2-game-windows.zip',
  'opennova-editor-windows.zip',
  'opennova-blender-addon-windows.zip',
  'opennova-apps-windows.zip',
  'opennova-switcher-windows.zip',
];

async function mountPage() {
  const router = await testRouter('/');
  const wrapper = mount(LandingPage, { global: { plugins: [router] } });
  await flushPromises();
  return wrapper;
}

function hrefs(wrapper: Awaited<ReturnType<typeof mountPage>>) {
  return wrapper.findAll('a').map((a) => a.attributes('href'));
}

// The line under a build's file name on its card: its version, size and SHA-256.
function buildLine(wrapper: Awaited<ReturnType<typeof mountPage>>, file: string) {
  return wrapper.get(`[data-test="build-${file}"]`).findAll('p')[1]?.text();
}

describe('LandingPage', () => {
  beforeEach(() => {
    vi.clearAllMocks();
    stats.mockResolvedValue({ games: 3, lobbies: 2, players: 9, updatedAtMs: 0 });
    tiers.mockResolvedValue({ ok: true, value: tiersResponse() });
    host.mockReturnValue(null);
    manifest.mockResolvedValue(null);
  });

  it('leads with our own game, and still plays the Joint Operations you own', async () => {
    const wrapper = await mountPage();
    const text = wrapper.text();
    expect(wrapper.get('h1').text()).toBe('OpenNova');
    expect(text).toContain("A game on NovaLogic's engine, rebuilt in the open.");
    expect(text).toContain('OP: Acid Gambit');
    expect(text).toContain('OP: Nifty Package');
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

  it("keeps the Switcher to the original dfx2.exe: Jointops.exe finds our servers on NovaLogic's NovaWorld", async () => {
    const text = (await mountPage()).text();
    expect(text).toContain('With OpenNova');
    expect(text).toContain("choose OpenNova, or Original NovaWorld for NovaLogic's. Nothing else to install.");
    expect(text).toContain('With the original Jointops.exe');
    expect(text).toContain("Joint Operations servers we host can also list on NovaLogic's NovaWorld");
    expect(text).toContain('With the original dfx2.exe');
    expect(text).toContain("NovaLogic's NovaWorld for Delta Force Xtreme 2 is down, so the OpenNova Switcher points");
    expect(text).toContain('gs.novaworld.net');
    expect(text).toContain("OpenNova and the original Jointops.exe don't need it.");
    // The Switcher is pitched for dfx2.exe alone, never for Jointops.exe.
    expect(text).not.toContain('Jointops.exe or dfx2.exe');
  });

  it('says hosted servers run our engine, and how to run one yourself for free', async () => {
    const wrapper = await mountPage();
    const text = wrapper.text();
    expect(text).toContain("Hosted servers run OpenNova's own server, opennova-serve, not the original game.");
    expect(text).toContain(
      "A Joint Operations server can also list on NovaLogic's NovaWorld, reaching players on the original game with nothing to install.",
    );
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

  it('lists the tiers with their prices and the dedicated machine', async () => {
    const wrapper = await mountPage();
    const text = wrapper.text();
    expect(text).toContain('$5');
    expect(text).toContain('$10');
    expect(text).toContain('$15');
    expect(text).toContain('1 hosted server');
    expect(text).toContain('3 hosted servers');
    expect(text).toContain('Dedicated machine: $45 a month for each, one more hosted server on a machine of its own, no tier needed.');
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
    expect(wrapper.text()).not.toContain('Dedicated machine:');
  });

  it("downloads every build from the deployment's downloads host, and says what each build is", async () => {
    host.mockReturnValue('https://downloads.example.com');
    const commit = '2d23f67' + 'a'.repeat(33);
    const sha = 'e36a8dab2612dab5e7deb7dfcd5388993336c442abead86589c4a82a751856f6';
    // A staging set: a pull request's debug builds, the game zips and the editor versioned.
    manifest.mockResolvedValue({
      version: 'PR #1260 (2d23f67)',
      published: '2026-10-26T01:56:58Z',
      source: 'https://github.com/opennova-net/opennova/actions/runs/40255180333',
      commit,
      build: 'debug',
      talks_to: 'staging',
      files: Object.fromEntries(
        BUILDS.map((file) => [
          file,
          { size: 53_178_900, sha256: sha, ...(/game|editor/.test(file) ? { version: '0.0.20' } : {}) },
        ]),
      ),
    });
    const wrapper = await mountPage();
    expect(manifest).toHaveBeenCalledWith('https://downloads.example.com');
    const buttons = wrapper.findAll('a').filter((a) => a.text() === 'Download' && a.attributes('aria-label'));
    expect(buttons.map((a) => a.attributes('href'))).toEqual(BUILDS.map((file) => `https://downloads.example.com/${file}`));
    expect(buttons[1].attributes('aria-label')).toBe('Download OpenNova for Joint Operations');

    const line = wrapper.get('[data-test="downloads-version"]');
    expect(line.text()).toBe(
      'Version PR #1260 (2d23f67), a debug build for staging from commit 2d23f67, published 2026-10-26 01:56 UTC.',
    );
    const commitLink = line.get('a');
    expect(commitLink.attributes('href')).toBe('https://github.com/opennova-net/opennova/actions/runs/40255180333');
    expect(commitLink.attributes('title')).toBe(commit);
    expect(line.get('time').attributes('datetime')).toBe('2026-10-26T01:56:58.000Z');

    // Each card: the file's name, then its version, the set's commit and the deployment a debug build
    // talks to (as the runtime's main menu names its build), size and short SHA-256 (the full one on hover).
    const game = wrapper.get('[data-test="build-jointops-game-windows.zip"]');
    expect(game.findAll('p').map((p) => p.text())).toEqual([
      'jointops-game-windows.zip',
      'v0.0.20 (2d23f67, staging), 53 MB, SHA-256 e36a8dab2612',
    ]);
    expect(game.get('span').attributes('title')).toBe(sha);
    // The tools' zips but the editor carry no version: the commit and the deployment alone.
    expect(buildLine(wrapper, 'opennova-apps-windows.zip')).toBe('2d23f67, staging, 53 MB, SHA-256 e36a8dab2612');
    expect(buildLine(wrapper, 'opennova-editor-windows.zip')).toBe('v0.0.20 (2d23f67, staging), 53 MB, SHA-256 e36a8dab2612');
    // GitHub sits in the footer now, not in the downloads.
    expect(hrefs(wrapper)).not.toContain('https://github.com/opennova-net/opennova/releases/latest');
  });

  it('calls a tagged set a release build', async () => {
    host.mockReturnValue('https://downloads.example.com');
    manifest.mockResolvedValue({
      version: 'v0.0.21',
      published: '2026-10-26T12:00:00Z',
      source: 'https://github.com/opennova-net/opennova/releases/tag/v0.0.21',
      commit: '1a2b3c4' + 'b'.repeat(33),
      build: 'release',
      talks_to: 'prod',
      files: Object.fromEntries(BUILDS.map((file) => [file, { size: 1_096_841, sha256: 'ab'.repeat(32), version: '0.0.21' }])),
    });
    const wrapper = await mountPage();
    const line = wrapper.get('[data-test="downloads-version"]');
    expect(line.text()).toBe('Version v0.0.21, a release build from commit 1a2b3c4, published 2026-10-26 12:00 UTC.');
    expect(line.get('a').attributes('href')).toBe('https://github.com/opennova-net/opennova/releases/tag/v0.0.21');
    // A release's cards name its commit and no deployment.
    expect(buildLine(wrapper, 'jointops-game-windows.zip')).toBe('v0.0.21 (1a2b3c4), 1.1 MB, SHA-256 abababababab');
  });

  it('shows what a manifest from before the build fields has', async () => {
    host.mockReturnValue('https://downloads.example.com');
    manifest.mockResolvedValue({
      version: 'PR #1260 (2d23f67)',
      published: '2026-10-26T01:56:58Z',
      source: 'https://github.com/opennova-net/opennova/actions/runs/40255180333',
      files: Object.fromEntries(BUILDS.map((file) => [file, { size: 53_185_054, sha256: 'ab'.repeat(32) }])),
    });
    const wrapper = await mountPage();
    const line = wrapper.get('[data-test="downloads-version"]');
    expect(line.text()).toBe('Version PR #1260 (2d23f67), published 2026-10-26 01:56 UTC.');
    expect(line.find('a').exists()).toBe(false);
    expect(buildLine(wrapper, 'opennova-game-windows.zip')).toBe('53 MB, SHA-256 abababababab');
  });

  it('offers no download with no downloads host, and still names every build', async () => {
    const wrapper = await mountPage();
    expect(manifest).not.toHaveBeenCalled();
    expect(wrapper.find('[data-test="downloads-version"]').exists()).toBe(false);
    const text = wrapper.text();
    for (const file of BUILDS) expect(text).toContain(file);
    expect(wrapper.findAll('a').filter((a) => a.text() === 'Download' && a.attributes('aria-label'))).toHaveLength(0);
    expect(wrapper.findAll('[aria-disabled="true"]').filter((s) => s.text() === 'Not published yet')).toHaveLength(BUILDS.length);
    expect(hrefs(wrapper).some((href) => href?.includes('/releases'))).toBe(false);
  });

  it("offers no download until the host's manifest lists the build", async () => {
    host.mockReturnValue('https://downloads.example.com');
    manifest.mockResolvedValue(null);
    const wrapper = await mountPage();
    expect(wrapper.findAll('a').filter((a) => a.text() === 'Download' && a.attributes('aria-label'))).toHaveLength(0);
    expect(wrapper.findAll('[aria-disabled="true"]').filter((s) => s.text() === 'Not published yet')).toHaveLength(BUILDS.length);
  });

  it('offers the account and lobby steps', async () => {
    const links = hrefs(await mountPage());
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
