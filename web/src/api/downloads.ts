// The deployment's downloads host (ADR 0070; infra/aws/downloads.tf): the builds the
// download buttons fetch, each under its stable name, beside manifest.json, which
// .github/workflows/publish.yml writes (scripts/stage_downloads.py).

// What manifest.json holds: the version the host serves, when it was published, where the
// builds came from, and each file's size and SHA-256.
export interface DownloadsManifest {
  version: string;
  published: string;
  source: string;
  files: Record<string, { size: number; sha256: string }>;
}

// The downloads host's base URL for the site at `hostname`: the deployment's own,
// downloads.<the site's hostname> (prod's site opennova.net and www.opennova.net take
// downloads.opennova.net, staging's staging.opennova.net takes downloads.staging.opennova.net),
// so the one portal image serves either deployment. null for a site with no downloads host (a
// local stack on localhost, an address), whose buttons fall back to the GitHub releases.
export function downloadsBase(hostname: string): string | null {
  const host = hostname.toLowerCase().replace(/\.$/, '');
  if (!host.includes('.') || /^[\d.]+$/.test(host) || host.includes(':') || host.endsWith('.localhost')) {
    return null;
  }
  return `https://downloads.${host.replace(/^www\./, '')}`;
}

function isManifest(value: unknown): value is DownloadsManifest {
  if (typeof value !== 'object' || value === null) return false;
  const m = value as Record<string, unknown>;
  if (typeof m.version !== 'string' || typeof m.published !== 'string' || typeof m.files !== 'object' || m.files === null) {
    return false;
  }
  return Object.values(m.files as Record<string, unknown>).every((file) => {
    const f = file as Record<string, unknown> | null;
    return f !== null && typeof f === 'object' && typeof f.size === 'number' && typeof f.sha256 === 'string';
  });
}

// The host's manifest, or null when it cannot be read (the host not up yet, offline): the
// buttons work without it, which only adds the version and the sizes.
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
