import { flushPromises, mount } from '@vue/test-utils';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import DownloadsPage from './DownloadsPage.vue';
import { downloadsBase, fetchDownloadsManifest } from '../api/downloads';
import { testRouter } from '../test/router';

vi.mock('../api/downloads', () => ({ downloadsBase: vi.fn(), fetchDownloadsManifest: vi.fn() }));


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
  const router = await testRouter('/downloads');
  const wrapper = mount(DownloadsPage, { global: { plugins: [router] } });
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


describe('DownloadsPage', () => {
  beforeEach(() => {
    vi.clearAllMocks();
    host.mockReturnValue(null);
    manifest.mockResolvedValue(null);
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
    expect(wrapper.findAll('[aria-disabled="true"]').filter((s) => s.text() === 'Unavailable')).toHaveLength(BUILDS.length);
    expect(hrefs(wrapper).some((href) => href?.includes('/releases'))).toBe(false);
  });

  it("offers no download until the host's manifest lists the build", async () => {
    host.mockReturnValue('https://downloads.example.com');
    manifest.mockResolvedValue(null);
    const wrapper = await mountPage();
    expect(wrapper.findAll('a').filter((a) => a.text() === 'Download' && a.attributes('aria-label'))).toHaveLength(0);
    expect(wrapper.findAll('[aria-disabled="true"]').filter((s) => s.text() === 'Unavailable')).toHaveLength(BUILDS.length);
    expect(wrapper.text()).toContain('The downloads manifest could not be loaded.');
  });

  it('distinguishes unpublished files from an unavailable manifest', async () => {
    host.mockReturnValue('https://downloads.example.com');
    manifest.mockResolvedValue({
      version: 'v0.0.21', published: '2026-10-26T12:00:00Z', source: 'https://github.com/opennova-net/opennova/releases/tag/v0.0.21',
      files: { [BUILDS[0]]: { size: 1000, sha256: 'ab'.repeat(32) } },
    });
    const page = await mountPage();
    expect(page.findAll('a[aria-label^="Download"]')).toHaveLength(1);
    expect(page.findAll('[aria-disabled="true"]').every((item) => item.text() === 'Not published yet')).toBe(true);
    expect(page.findAll('[aria-disabled="true"]')).toHaveLength(BUILDS.length - 1);
  });

});
