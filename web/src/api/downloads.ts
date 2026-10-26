// The deployment's downloads host (ADR 0070; infra/aws/downloads.tf): the builds the
// download buttons fetch, each under its stable name, beside manifest.json, which
// .github/workflows/publish.yml writes (scripts/stage_downloads.py).

// One build on the host: its size, its SHA-256, and the version its file name carried (the
// game zips' and the editor's; the other tools' carry none).
export interface DownloadsFile {
  size: number;
  sha256: string;
  version?: string;
}

// What manifest.json holds: the version the host serves, when it was published, where the
// builds came from (a release's or a CI run's URL), the commit they were made from, the build
// kind (release or debug) and the deployment its builds talk to (prod or staging), and each
// file. A manifest published before the commit, the build kind and the files' versions were
// added has none of them.
export interface DownloadsManifest {
  version: string;
  published: string;
  source: string;
  commit?: string;
  build?: string;
  talks_to?: string;
  files: Record<string, DownloadsFile>;
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

function optionalString(value: unknown): boolean {
  return value === undefined || typeof value === 'string';
}

function isManifest(value: unknown): value is DownloadsManifest {
  if (typeof value !== 'object' || value === null) return false;
  const m = value as Record<string, unknown>;
  if (typeof m.version !== 'string' || typeof m.published !== 'string' || typeof m.files !== 'object' || m.files === null) {
    return false;
  }
  if (!optionalString(m.commit) || !optionalString(m.build) || !optionalString(m.talks_to)) return false;
  return Object.values(m.files as Record<string, unknown>).every((file) => {
    const f = file as Record<string, unknown> | null;
    return (
      f !== null && typeof f === 'object' && typeof f.size === 'number' && typeof f.sha256 === 'string' && optionalString(f.version)
    );
  });
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
