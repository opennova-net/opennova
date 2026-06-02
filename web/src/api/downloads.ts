import axios from 'axios';
import type { DownloadManifest } from '../types/downloads';

const DEFAULT_LAUNCHER_MANIFEST = 'https://downloads.opennova.net/launcher/app.json';

export async function fetchLauncherManifest(signal?: AbortSignal): Promise<DownloadManifest> {
  return fetchManifest(
    import.meta.env.VITE_LAUNCHER_MANIFEST_URL || DEFAULT_LAUNCHER_MANIFEST,
    signal,
  );
}

async function fetchManifest(url: string, signal?: AbortSignal): Promise<DownloadManifest> {
  const response = await axios.get(url, { signal });
  return normalizeManifest(response.data);
}

function normalizeManifest(data: unknown): DownloadManifest {
  if (!isRecord(data)) {
    throw new Error('Invalid launcher manifest format');
  }

  const downloadUrl = pickString(
    data.download_url,
    data.url,
    data.href,
  );

  if (!downloadUrl) {
    throw new Error('Launcher manifest missing download_url');
  }

  const platform = pickString(data.platform) ?? 'Windows';
  const version = pickString(data.version) ?? 'unknown';
  const filename = pickString(data.filename) ?? 'download.zip';

  return {
    platform,
    version,
    filename,
    downloadUrl,
    sizeHuman: pickString(data.size_human) ?? pickString(data.sizeLabel),
    sizeBytes: typeof data.size === 'number' ? data.size : undefined,
    releaseDate: pickString(data.release_date) ?? pickString(data.releaseDate),
    architecture: pickString(data.architecture) ?? pickString(data.arch),
    description: pickString(data.description),
    releaseNotes: pickString(data.release_notes) ?? pickString(data.releaseNotes),
    heroImage: pickString(data.hero_image) ?? pickString(data.heroImage),
  };
}

function isRecord(value: unknown): value is Record<string, any> {
  return !!value && typeof value === 'object' && !Array.isArray(value);
}

function pickString(...candidates: unknown[]): string | undefined {
  for (const candidate of candidates) {
    if (typeof candidate === 'string' && candidate.trim().length > 0) {
      return candidate;
    }
  }
  return undefined;
}
