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
    commit: '2d23f67' + '0'.repeat(33),
    build: 'release',
    talks_to: 'prod',
    files: { 'opennova-game-windows.zip': { size: 53178900, sha256: 'ab'.repeat(32) } },
  };

  it("reads the host's manifest", async () => {
    const fetchMock = vi.fn().mockResolvedValue(new Response(JSON.stringify(manifest), { status: 200 }));
    vi.stubGlobal('fetch', fetchMock);
    expect(await fetchDownloadsManifest('https://downloads.opennova.net')).toEqual(manifest);
    expect(fetchMock.mock.calls[0][0]).toBe('https://downloads.opennova.net/manifest.json');
  });

  it("reads the files' versions, and needs the commit, the build kind and what it talks to", async () => {
    const full = {
      ...manifest,
      files: {
        'opennova-game-windows.zip': { size: 53178900, sha256: 'ab'.repeat(32), version: '0.0.21' },
        'opennova-apps-windows.zip': { size: 5120318, sha256: 'cd'.repeat(32) },
      },
    };
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue(new Response(JSON.stringify(full), { status: 200 })));
    expect(await fetchDownloadsManifest('https://downloads.opennova.net')).toEqual(full);
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue(new Response(JSON.stringify({ ...full, commit: 7 }), { status: 200 })));
    expect(await fetchDownloadsManifest('https://downloads.opennova.net')).toBeNull();
    for (const field of ['commit', 'build', 'talks_to']) {
      const missing: Record<string, unknown> = { ...full };
      delete missing[field];
      vi.stubGlobal('fetch', vi.fn().mockResolvedValue(new Response(JSON.stringify(missing), { status: 200 })));
      expect(await fetchDownloadsManifest('https://downloads.opennova.net')).toBeNull();
    }
    const badVersion = { ...manifest, files: { 'opennova-game-windows.zip': { size: 1, sha256: 'ab', version: 20 } } };
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue(new Response(JSON.stringify(badVersion), { status: 200 })));
    expect(await fetchDownloadsManifest('https://downloads.opennova.net')).toBeNull();
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
