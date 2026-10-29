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
    // The service's games name the protocol their titles speak.
    protocol: key === 'JO' || key === 'ACIDGAMBIT' ? { code: 'jo', name: 'JO', version: '16' } : key === 'DFX2' ? { code: 'dfx2', name: 'DFX2', version: '16' } : null,
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
    expect(wrapper.get('h1').text()).toBe('Downloads');
    // Games, then apps, each in the manifest's order; a product with no file is not shown.
    expect(wrapper.findAll('li h3').map((h) => h.text())).toEqual(['OP: Acid Gambit', 'Joint Operations', 'Delta Force Xtreme 2',
      'OpenNova Editor', 'OpenNova tools', 'Blender add-on']);
    expect(wrapper.findAll('h2').map((h) => h.text())).toEqual(['Games', 'Apps']);
    expect(wrapper.text()).not.toContain('OpenNova Runtime');
    expect(wrapper.text()).not.toContain('Escalation');
    // A published product's button fetches its file from the host; one with no release yet waits.
    expect(downloadButtons(wrapper).map((a) => a.attributes('href'))).toEqual([
      'https://downloads.example.com/opennova-game-windows.zip',
      'https://downloads.example.com/jointops-game-windows.zip',
      'https://downloads.example.com/opennova-editor-windows.zip',
      'https://downloads.example.com/opennova-tools-windows.zip',
    ]);
    expect(downloadButtons(wrapper)[1].attributes('aria-label')).toBe('Download Joint Operations');
    expect(wrapper.findAll('[aria-disabled="true"]').map((s) => s.text())).toEqual(['Not published yet', 'Not published yet']);
    // Each card: name, version and size, Download; the file, SHA-256 and publish time under Details.
    const game = wrapper.get('#jointops');
    expect(game.get('[data-test="facts"]').text()).toBe('0.0.22.123 · JO protocol 16 · 53 MB');
    expect(wrapper.get('#oneditor [data-test="facts"]').text()).toBe('v0.0.22.123 · 53 MB');
    expect(game.text()).not.toContain('described');
    const details = game.get('details[data-test="build-jointops-game-windows.zip"]');
    expect(details.get('summary').text()).toBe('Details');
    expect(details.findAll('dd').map((d) => d.text())).toEqual(['jointops-game-windows.zip', SHA, '2026-10-27 12:00 UTC']);
    expect(wrapper.get('[data-test="build-dfx2-game-windows.zip"]').findAll('dd').map((d) => d.text())).toEqual(['dfx2-game-windows.zip']);
    expect(wrapper.find('#dfx2 [data-test="facts"]').exists()).toBe(false);
    // The Play page's anchors: each game by its zip, the apps' section.
    for (const id of ['opennova', 'jointops', 'dfx2', 'apps', 'games']) expect(wrapper.find(`#${id}`).exists()).toBe(true);
    const written = wrapper.get('[data-test="downloads-written"]');
    expect(written.get('time').attributes('datetime')).toBe('2026-10-27T12:05:00.000Z');
    expect(written.text()).toBe('Updated 2026-10-27 12:05 UTC');
    // GitHub sits in the footer, never in the downloads.
    expect(wrapper.findAll('a').some((a) => a.attributes('href')?.includes('/releases'))).toBe(false);
  });

  it('shows no products with no downloads host', async () => {
    const wrapper = await mountPage();
    expect(manifest).not.toHaveBeenCalled();
    expect(wrapper.find('[data-test="downloads-written"]').exists()).toBe(false);
    expect(wrapper.text()).toContain('Downloads are not configured for this site.');
    expect(wrapper.findAll('li h3')).toHaveLength(0);
  });

  it('says so when the manifest cannot be read', async () => {
    host.mockReturnValue('https://downloads.example.com');
    manifest.mockResolvedValue(null);
    const wrapper = await mountPage();
    expect(wrapper.get('.notice').text()).toContain('The downloads could not be read.');
    expect(downloadButtons(wrapper)).toHaveLength(0);
  });

  it('leaves out a section with nothing to offer', async () => {
    host.mockReturnValue('https://downloads.example.com');
    manifest.mockResolvedValue({ written: '2026-10-27T12:05:00Z', products: PRODUCTS.filter((p) => p.kind === 'app') });
    const wrapper = await mountPage();
    expect(wrapper.findAll('h2').map((h) => h.text())).toEqual(['Apps']);
  });

  it("lists the games players published apart from ours, each with its owner", async () => {
    host.mockReturnValue('https://downloads.example.com');
    const rangers = { ...product('RANGERS', 'game', 'Rangers', 'player-rangers-game-windows.zip'), description: '', owner: 'biggy', version: '1.0' };
    const jointops = { ...product('JOINTOPS', 'game', 'Joint Ops Redux', 'player-jointops-game-windows.zip'), owner: 'kilo' };
    manifest.mockResolvedValue({
      written: '2026-10-27T12:05:00Z',
      products: [{ ...product('JO', 'game', 'Joint Operations', 'jointops-game-windows.zip'), owner: null }, rangers, jointops, product('ONEDITOR', 'app', 'OpenNova Editor', 'opennova-editor-windows.zip')],
    });
    const wrapper = await mountPage();
    expect(wrapper.findAll('h2').map((h) => h.text())).toEqual(['Games', 'Apps', "Players' games"]);
    const cards = wrapper.findAll('li').filter((li) => li.find('[data-test="owner"]').exists());
    expect(cards.map((c) => [c.get('h3').text(), c.get('[data-test="owner"]').text()])).toEqual([
      ['Rangers', 'by biggy'],
      ['Joint Ops Redux', 'by kilo'],
    ]);
    // Each answers to its zip's name, apart from a game of ours its key may spell.
    expect(cards[0].attributes('id')).toBe('player-rangers');
    expect(cards[1].attributes('id')).toBe('player-jointops');
    expect(wrapper.find('#jointops h3').text()).toBe('Joint Operations');
    expect(cards[0].get('a').attributes('href')).toBe('https://downloads.example.com/player-rangers-game-windows.zip');
    expect(cards[0].get('[data-test="facts"]').text()).toBe('1.0 · 53 MB');
  });

  it("shows no players' games section when the manifest names no owner", async () => {
    host.mockReturnValue('https://downloads.example.com');
    manifest.mockResolvedValue({ written: '2026-10-27T12:05:00Z', products: [product('RANGERS', 'game', 'Rangers', 'player-rangers-game-windows.zip')] });
    const wrapper = await mountPage();
    expect(wrapper.findAll('h2').map((h) => h.text())).toEqual(['Games']);
    expect(wrapper.find('#players-games').exists()).toBe(false);
  });
});
