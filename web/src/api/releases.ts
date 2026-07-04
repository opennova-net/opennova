import axios from 'axios';
import type { ToolAsset, ToolOs, ToolRelease } from '../types/downloads';

// Tool deliverables (editor, runtime, importer, exporters) publish as GitHub Release
// assets on this repo — not to S3 (only the launcher rides S3, see api/downloads.ts).
// We list releases and pick the latest tool tag, skipping launcher releases whose tags
// are prefixed "launcher-" (so GitHub's /releases/latest can't accidentally surface one).
const RELEASES_URL =
  'https://api.github.com/repos/opennova-net/opennova/releases?per_page=30';

interface GithubAsset {
  name: string;
  size: number;
  browser_download_url: string;
}

interface GithubRelease {
  tag_name: string;
  html_url: string;
  draft: boolean;
  prerelease: boolean;
  assets: GithubAsset[];
}

/**
 * Maps a release-asset filename to its presentation metadata. Names are stable and
 * version-suffixed; canonical naming lives in release/deliverables.toml. Returns null
 * for anything unrecognized (defensive against future additions).
 */
function classifyAsset(name: string): Omit<ToolAsset, 'filename' | 'sizeBytes' | 'sizeHuman' | 'downloadUrl'> | null {
  const rules: Array<{ re: RegExp; meta: Omit<ToolAsset, 'filename' | 'sizeBytes' | 'sizeHuman' | 'downloadUrl'> }> = [
    { re: /^opennova-modding-editor-windows-/, meta: { product: 'OpenNova Editor (ONED)', os: 'windows', kind: 'app' } },
    { re: /^opennova-modding-editor-macos-/, meta: { product: 'OpenNova Editor (ONED)', os: 'macos', kind: 'app' } },
    { re: /^opennova-game-runtime-windows-/, meta: { product: 'Game Runtime', os: 'windows', kind: 'app' } },
    { re: /^opennova-game-runtime-macos-/, meta: { product: 'Game Runtime', os: 'macos', kind: 'app' } },
    { re: /^opennova-asset-importer-windows-/, meta: { product: 'Asset Importer', os: 'windows', kind: 'tool' } },
    { re: /^opennova-3ds-max-ase-exporter-windows-/, meta: { product: '3ds Max ASE Exporter', os: 'windows', kind: 'plugin' } },
    { re: /^opennova-blender-ase-exporter-/, meta: { product: 'Blender ASE Exporter', os: 'any', kind: 'plugin' } },
  ];
  for (const { re, meta } of rules) {
    if (re.test(name)) {
      return meta;
    }
  }
  return null;
}

/** Formats a raw byte count as a compact human string (GitHub gives raw bytes). */
export function formatBytes(bytes: number): string {
  if (!Number.isFinite(bytes) || bytes <= 0) {
    return '';
  }
  const units = ['B', 'KB', 'MB', 'GB'];
  let value = bytes;
  let unit = 0;
  while (value >= 1024 && unit < units.length - 1) {
    value /= 1024;
    unit += 1;
  }
  const rounded = value >= 100 || unit === 0 ? Math.round(value) : Math.round(value * 10) / 10;
  return `${rounded} ${units[unit]}`;
}

function isToolRelease(release: GithubRelease): boolean {
  const tag = release.tag_name || '';
  return !release.draft && !release.prerelease && /^v\d/.test(tag) && !tag.startsWith('launcher');
}

/**
 * Fetches the latest tool release from the GitHub API and returns its recognized
 * deliverables as direct downloads. Ordered apps-first for a stable presentation.
 *
 * Note: unauthenticated GitHub API allows 60 req/hr per IP — one request per page load
 * is fine; cache client-side or proxy via the backend if that ever becomes a concern.
 */
export async function fetchLatestToolRelease(signal?: AbortSignal): Promise<ToolRelease> {
  const response = await axios.get<GithubRelease[]>(RELEASES_URL, {
    signal,
    headers: { Accept: 'application/vnd.github+json' },
  });

  const releases = Array.isArray(response.data) ? response.data : [];
  const latest = releases.find(isToolRelease);
  if (!latest) {
    throw new Error('No tool release found');
  }

  const kindOrder: Record<ToolAsset['kind'], number> = { app: 0, tool: 1, plugin: 2 };
  const assets: ToolAsset[] = latest.assets
    .map((asset): ToolAsset | null => {
      const meta = classifyAsset(asset.name);
      if (!meta) {
        return null;
      }
      return {
        ...meta,
        filename: asset.name,
        sizeBytes: asset.size,
        sizeHuman: formatBytes(asset.size),
        downloadUrl: asset.browser_download_url,
      };
    })
    .filter((asset): asset is ToolAsset => asset !== null)
    .sort((a, b) => kindOrder[a.kind] - kindOrder[b.kind] || a.product.localeCompare(b.product));

  if (assets.length === 0) {
    throw new Error('Latest release has no recognized tool downloads');
  }

  return {
    version: latest.tag_name.replace(/^v/, ''),
    htmlUrl: latest.html_url,
    assets,
  };
}

/** Detects the visitor's OS from the browser, defaulting to Windows when unknown. */
export function detectOs(): Exclude<ToolOs, 'any'> {
  const nav = typeof navigator !== 'undefined' ? navigator : undefined;
  if (!nav) {
    return 'windows';
  }
  const uaData = (nav as unknown as { userAgentData?: { platform?: string } }).userAgentData;
  const hint = `${uaData?.platform ?? ''} ${nav.platform ?? ''} ${nav.userAgent ?? ''}`.toLowerCase();
  if (hint.includes('mac')) {
    return 'macos';
  }
  if (hint.includes('win')) {
    return 'windows';
  }
  return 'windows';
}
