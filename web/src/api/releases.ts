import axios from 'axios';
import type { GameRelease, ReleaseAsset } from '../types/downloads';

// Game deliverables publish as GitHub Release assets on this repo, not
// to S3 (only OpenNova Launcher rides S3, see api/downloads.ts). We skip its
// "launcher-" tags so GitHub's /releases/latest cannot surface the wrong product.
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
 * version-suffixed. Returns null for anything unrecognized.
 */
function classifyAsset(name: string): Pick<ReleaseAsset, 'product'> | null {
  return /^opennova-game-windows-/.test(name) ? { product: 'OpenNova Game' } : null;
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

function isGameRelease(release: GithubRelease): boolean {
  const tag = release.tag_name || '';
  return !release.draft && !release.prerelease && /^v\d/.test(tag) && !tag.startsWith('launcher');
}

/**
 * Fetches the latest game release from the GitHub API and returns its recognized
 * deliverables as direct downloads.
 *
 * Note: unauthenticated GitHub API allows 60 req/hr per IP — one request per page load
 * is fine; cache client-side or proxy via the backend if that ever becomes a concern.
 */
export async function fetchLatestGameRelease(signal?: AbortSignal): Promise<GameRelease> {
  const response = await axios.get<GithubRelease[]>(RELEASES_URL, {
    signal,
    headers: { Accept: 'application/vnd.github+json' },
  });

  const releases = Array.isArray(response.data) ? response.data : [];
  const latest = releases.find(isGameRelease);
  if (!latest) {
    throw new Error('No game release found');
  }

  const assets: ReleaseAsset[] = latest.assets
    .map((asset): ReleaseAsset | null => {
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
    .filter((asset): asset is ReleaseAsset => asset !== null);

  if (assets.length === 0) {
    throw new Error('Latest release has no recognized game download');
  }

  return {
    version: latest.tag_name.replace(/^v/, ''),
    htmlUrl: latest.html_url,
    assets,
  };
}
