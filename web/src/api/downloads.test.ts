import { afterEach, describe, expect, it, vi } from 'vitest';
import { downloadsBase, fetchDownloadsManifest } from './downloads';

describe('downloadsBase', () => {
  it("names each deployment's own host from the site's hostname", () => {
    expect(downloadsBase('opennova.net')).toBe('https://downloads.opennova.net');
    expect(downloadsBase('www.opennova.net')).toBe('https://downloads.opennova.net');
    expect(downloadsBase('staging.opennova.net')).toBe('https://downloads.staging.opennova.net');
    expect(downloadsBase('Example.COM.')).toBe('https://downloads.example.com');
  });

  it('has none for a local stack or an address', () => {
    expect(downloadsBase('localhost')).toBeNull();
    expect(downloadsBase('app.localhost')).toBeNull();
    expect(downloadsBase('127.0.0.1')).toBeNull();
    expect(downloadsBase('::1')).toBeNull();
  });
});

describe('fetchDownloadsManifest', () => {
  afterEach(() => {
    vi.unstubAllGlobals();
  });

  const published = {
    key: 'JO',
    kind: 'game',
    name: 'Joint Operations',
    description: 'Joint Operations and Escalation.',
    file: 'jointops-game-windows.zip',
    size: 53178900,
    sha256: 'ab'.repeat(32),
    version: '0.0.22',
    release: 12,
    published: '2026-10-27T12:00:00Z',
  };
  // The runtime: no download of its own, and its release named all the same.
  const runtime = {
    ...published,
    key: 'ONRUNTIME',
    kind: 'app',
    name: 'OpenNova Runtime',
    file: null,
    size: null,
    sha256: null,
    release: 'OpenNova Runtime v0.0.22',
  };
  // A product with a download and no published release yet.
  const unpublished = {
    ...published,
    key: 'ONTOOLS',
    kind: 'app',
    file: 'opennova-tools-windows.zip',
    size: null,
    sha256: null,
    version: null,
    release: null,
    published: null,
  };
  const manifest = { written: '2026-10-27T12:01:00Z', products: [published, runtime, unpublished] };

  function serving(body: unknown) {
    const fetchMock = vi.fn().mockResolvedValue(new Response(JSON.stringify(body), { status: 200 }));
    vi.stubGlobal('fetch', fetchMock);
    return fetchMock;
  }

  it("reads the host's manifest: every product, published or not", async () => {
    const fetchMock = serving(manifest);
    expect(await fetchDownloadsManifest('https://downloads.opennova.net')).toEqual(manifest);
    expect(fetchMock.mock.calls[0][0]).toBe('https://downloads.opennova.net/manifest.json');
  });

  it('refuses a product of another shape', async () => {
    serving({ ...manifest, written: 7 });
    expect(await fetchDownloadsManifest('https://downloads.opennova.net')).toBeNull();
    serving({ written: manifest.written, products: {} });
    expect(await fetchDownloadsManifest('https://downloads.opennova.net')).toBeNull();
    for (const [field, value] of [['kind', 'mod'], ['name', null], ['file', 3], ['size', '5 MB'], ['sha256', 1],
                                   ['version', 22], ['release', true], ['published', 0]] as const) {
      serving({ ...manifest, products: [{ ...published, [field]: value }] });
      expect(await fetchDownloadsManifest('https://downloads.opennova.net')).toBeNull();
    }
    const missing: Record<string, unknown> = { ...published };
    delete missing.description;
    serving({ ...manifest, products: [missing] });
    expect(await fetchDownloadsManifest('https://downloads.opennova.net')).toBeNull();
  });

  it('answers null for a missing host, a failed request or a body of another shape', async () => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue(new Response('no', { status: 404 })));
    expect(await fetchDownloadsManifest('https://downloads.opennova.net')).toBeNull();
    vi.stubGlobal('fetch', vi.fn().mockRejectedValue(new TypeError('offline')));
    expect(await fetchDownloadsManifest('https://downloads.opennova.net')).toBeNull();
    serving({ version: 1 });
    expect(await fetchDownloadsManifest('https://downloads.opennova.net')).toBeNull();
  });
});
