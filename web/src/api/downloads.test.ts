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

  const manifest = {
    version: 'v0.0.21',
    published: '2026-10-26T12:00:00Z',
    source: 'https://github.com/opennova-net/opennova/releases/tag/v0.0.21',
    files: { 'opennova-game-windows.zip': { size: 53178900, sha256: 'ab'.repeat(32) } },
  };

  it("reads the host's manifest", async () => {
    const fetchMock = vi.fn().mockResolvedValue(new Response(JSON.stringify(manifest), { status: 200 }));
    vi.stubGlobal('fetch', fetchMock);
    expect(await fetchDownloadsManifest('https://downloads.opennova.net')).toEqual(manifest);
    expect(fetchMock.mock.calls[0][0]).toBe('https://downloads.opennova.net/manifest.json');
  });

  it('answers null for a missing host, a failed request or a body of another shape', async () => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue(new Response('no', { status: 404 })));
    expect(await fetchDownloadsManifest('https://downloads.opennova.net')).toBeNull();
    vi.stubGlobal('fetch', vi.fn().mockRejectedValue(new TypeError('offline')));
    expect(await fetchDownloadsManifest('https://downloads.opennova.net')).toBeNull();
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue(new Response(JSON.stringify({ version: 1 }), { status: 200 })));
    expect(await fetchDownloadsManifest('https://downloads.opennova.net')).toBeNull();
  });
});
