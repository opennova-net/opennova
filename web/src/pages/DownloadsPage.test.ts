import { flushPromises, mount } from '@vue/test-utils';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import DownloadsPage from './DownloadsPage.vue';
import { downloadsBase, fetchDownloadsManifest, type DownloadsProduct } from '../api/downloads';
import { testRouter } from '../test/router';

vi.mock('../api/downloads', () => ({ downloadsBase: vi.fn(), fetchDownloadsManifest: vi.fn() }));

const host = vi.mocked(downloadsBase);
const manifest = vi.mocked(fetchDownloadsManifest);

const SHA = 'e36a8dab2612dab5e7deb7dfcd5388993336c442abead86589c4a82a751856f6';

function product(key: string, kind: DownloadsProduct['kind'], name: string, file: string | null, published = true): DownloadsProduct {
  return {
    key,
    kind,
    name,
    description: `${name}, described.`,
    file,
    size: file && published ? 53_178_900 : null,
    sha256: file && published ? SHA : null,
    version: published ? '0.0.22.123' : null,
    release: published ? 40 : null,
    published: published ? '2026-10-27T12:00:00Z' : null,
  };
}

// A deployment's products in the manifest's order: the apps listed before a game, as the data
// program may list them, and the products with no download of their own among them.
const PRODUCTS = [
  product('ONRUNTIME', 'app', 'OpenNova Runtime', null),
  product('ONEDITOR', 'app', 'OpenNova Editor', 'opennova-editor-windows.zip'),
  product('ACIDGAMBIT', 'game', 'OP: Acid Gambit', 'opennova-game-windows.zip'),
  product('NIFTY', 'expansion', 'OP: Nifty Package', null),
  product('JO', 'game', 'Joint Operations', 'jointops-game-windows.zip'),
  product('JOX01', 'expansion', 'Escalation', null),
  product('DFX2', 'game', 'Delta Force Xtreme 2', 'dfx2-game-windows.zip', false),
  product('ONTOOLS', 'app', 'OpenNova tools', 'opennova-tools-windows.zip'),
  product('ONBLENDER', 'app', 'Blender add-on', 'opennova-blender-addon-windows.zip', false),
  product('ONSWITCHER', 'app', 'OpenNova Switcher', 'opennova-switcher-windows.zip'),
];

async function mountPage() {
  const router = await testRouter('/downloads');
  const wrapper = mount(DownloadsPage, { global: { plugins: [router] } });
  await flushPromises();
  return wrapper;
}

function downloadButtons(wrapper: Awaited<ReturnType<typeof mountPage>>) {
  return wrapper.findAll('a').filter((a) => a.text() === 'Download' && a.attributes('aria-label'));
}

describe('DownloadsPage', () => {
  beforeEach(() => {
    vi.clearAllMocks();
    host.mockReturnValue(null);
    manifest.mockResolvedValue(null);
  });

  it("offers each product's published release from the deployment's downloads host, games first", async () => {
    host.mockReturnValue('https://downloads.example.com');
    manifest.mockResolvedValue({ written: '2026-10-27T12:05:00Z', products: PRODUCTS });
    const wrapper = await mountPage();
    expect(manifest).toHaveBeenCalledWith('https://downloads.example.com');
    // Games, then apps, each in the manifest's order; a product with no file is not shown.
    const titles = wrapper.findAll('li h3').map((h) => h.text());
    expect(titles).toEqual(['OP: Acid Gambit', 'Joint Operations', 'Delta Force Xtreme 2', 'OpenNova Editor',
      'OpenNova tools', 'Blender add-on', 'OpenNova Switcher']);
    expect(wrapper.findAll('h2').map((h) => h.text())).toEqual(['Games', 'Apps']);
    expect(wrapper.text()).not.toContain('OpenNova Runtime');
    expect(wrapper.text()).not.toContain('Escalation');
    // A published product's button fetches its file from the host; one with a file and no release yet waits.
    expect(downloadButtons(wrapper).map((a) => a.attributes('href'))).toEqual([
      'https://downloads.example.com/opennova-game-windows.zip',
      'https://downloads.example.com/jointops-game-windows.zip',
      'https://downloads.example.com/opennova-editor-windows.zip',
      'https://downloads.example.com/opennova-tools-windows.zip',
      'https://downloads.example.com/opennova-switcher-windows.zip',
    ]);
    expect(downloadButtons(wrapper)[1].attributes('aria-label')).toBe('Download Joint Operations');
    const waiting = wrapper.findAll('[aria-disabled="true"]');
    expect(waiting.map((s) => s.text())).toEqual(['Not published yet', 'Not published yet']);
    // Each card: its description, its file's name, its release's version, size and short SHA-256 (the full
    // one on hover), and when it was published.
    const game = wrapper.get('#jointops');
    expect(game.text()).toContain('Joint Operations, described.');
    const lines = wrapper.get('[data-test="build-jointops-game-windows.zip"]').findAll('p').map((p) => p.text());
    expect(lines).toEqual(['jointops-game-windows.zip', 'v0.0.22.123, 53 MB, SHA-256 e36a8dab2612',
      'Published 2026-10-27 12:00 UTC']);
    expect(wrapper.get('[data-test="build-jointops-game-windows.zip"] span').attributes('title')).toBe(SHA);
    expect(wrapper.get('[data-test="build-dfx2-game-windows.zip"]').findAll('p').map((p) => p.text())).toEqual([
      'dfx2-game-windows.zip',
    ]);
    // The Play page's anchors: each game by its zip, the apps' section.
    for (const id of ['opennova', 'jointops', 'dfx2', 'apps', 'games']) expect(wrapper.find(`#${id}`).exists()).toBe(true);
    const written = wrapper.get('[data-test="downloads-written"]');
    expect(written.get('time').attributes('datetime')).toBe('2026-10-27T12:05:00.000Z');
    expect(written.text()).toContain('Updated 2026-10-27 12:05 UTC.');
    // GitHub sits in the footer, never in the downloads.
    expect(wrapper.findAll('a').some((a) => a.attributes('href')?.includes('/releases'))).toBe(false);
  });

  it('shows no products with no downloads host', async () => {
    const wrapper = await mountPage();
    expect(manifest).not.toHaveBeenCalled();
    expect(wrapper.find('[data-test="downloads-written"]').exists()).toBe(false);
    expect(wrapper.text()).toContain('Downloads are not configured for this site.');
    expect(wrapper.findAll('li h3')).toHaveLength(0);
    expect(downloadButtons(wrapper)).toHaveLength(0);
  });

  it('says so when the manifest cannot be read', async () => {
    host.mockReturnValue('https://downloads.example.com');
    manifest.mockResolvedValue(null);
    const wrapper = await mountPage();
    expect(wrapper.text()).toContain('The downloads manifest could not be loaded.');
    expect(downloadButtons(wrapper)).toHaveLength(0);
  });

  it('leaves out a section with nothing to offer', async () => {
    host.mockReturnValue('https://downloads.example.com');
    manifest.mockResolvedValue({ written: '2026-10-27T12:05:00Z', products: PRODUCTS.filter((p) => p.kind === 'app') });
    const wrapper = await mountPage();
    expect(wrapper.findAll('h2').map((h) => h.text())).toEqual(['Apps']);
  });
});
