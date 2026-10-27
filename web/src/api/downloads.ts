// The deployment's downloads host (ADR 0070, ADR 0072; infra/aws/downloads.tf): each product's
// published release, the ones with a download, under its stable name, beside manifest.json, which
// the data program writes whenever a release is published or retired (DEPLOY.md, "The downloads
// host").

// What a product is: a game, an expansion of one, or an app (the runtime, the editor, the tools).
export type ProductKind = 'game' | 'expansion' | 'app';

// One product as the manifest lists it: its key, kind, name and description, the username of
// the player whose game it is (null for the service's own; absent from a manifest written before
// players' games were listed), then its download: the file's stable name (null for a product the
// host offers none of: the runtime, an expansion a game's zip installs), and its size, SHA-256,
// version, release and when that was published, each null until the product has a published
// release. A player's game is listed once it is public with a published release, its file
// player-<key lowercased>-game-windows.zip (never the name of one of the service's zips).
export interface DownloadsProduct {
  key: string;
  kind: ProductKind;
  name: string;
  description: string;
  owner?: string | null;
  file: string | null;
  size: number | null;
  sha256: string | null;
  version: string | null;
  release: string | number | null;
  published: string | null;
}

// What manifest.json holds: when it was written, and every product of the deployment in its
// order.
export interface DownloadsManifest {
  written: string;
  products: DownloadsProduct[];
}

// The downloads host's base URL for the site at `hostname`: the deployment's own,
// downloads.<the site's hostname> (prod's site opennova.net and www.opennova.net take
// downloads.opennova.net, staging's staging.opennova.net takes downloads.staging.opennova.net),
// so the one portal image serves either deployment. null for a site with no downloads host (a
// local stack on localhost, an address), whose buttons read "Unavailable" (ADR 0070 d4: the site
// never links GitHub's releases for a build).
export function downloadsBase(hostname: string): string | null {
  const host = hostname.toLowerCase().replace(/\.$/, '');
  if (!host.includes('.') || /^[\d.]+$/.test(host) || host.includes(':') || host.endsWith('.localhost')) {
    return null;
  }
  return `https://downloads.${host.replace(/^www\./, '')}`;
}

const KINDS: readonly string[] = ['game', 'expansion', 'app'];

function stringOrNull(value: unknown): boolean {
  return value === null || typeof value === 'string';
}

function isProduct(value: unknown): value is DownloadsProduct {
  if (typeof value !== 'object' || value === null) return false;
  const p = value as Record<string, unknown>;
  return (
    typeof p.key === 'string' &&
    typeof p.kind === 'string' &&
    KINDS.includes(p.kind) &&
    typeof p.name === 'string' &&
    typeof p.description === 'string' &&
    (p.owner === undefined || stringOrNull(p.owner)) &&
    stringOrNull(p.file) &&
    (p.size === null || typeof p.size === 'number') &&
    stringOrNull(p.sha256) &&
    stringOrNull(p.version) &&
    (p.release === null || typeof p.release === 'string' || typeof p.release === 'number') &&
    stringOrNull(p.published)
  );
}

function isManifest(value: unknown): value is DownloadsManifest {
  if (typeof value !== 'object' || value === null) return false;
  const m = value as Record<string, unknown>;
  return typeof m.written === 'string' && Array.isArray(m.products) && m.products.every(isProduct);
}

// The host's manifest, or null when it cannot be read (the host not up yet, offline), when the
// page offers no download.
export async function fetchDownloadsManifest(base: string, signal?: AbortSignal): Promise<DownloadsManifest | null> {
  try {
    const response = await fetch(`${base}/manifest.json`, { signal, cache: 'no-cache' });
    if (!response.ok) return null;
    const body: unknown = await response.json();
    return isManifest(body) ? body : null;
  } catch {
    return null;
  }
}
