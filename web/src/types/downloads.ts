export interface DownloadManifest {
  platform: string;
  version: string;
  filename: string;
  downloadUrl: string;
  sizeHuman?: string;
  sizeBytes?: number;
  releaseDate?: string;
  architecture?: string;
  description?: string;
  releaseNotes?: string;
  heroImage?: string;
}

/** A single downloadable deliverable from a GitHub Release. */
export interface ReleaseAsset {
  /** Human product name, e.g. "OpenNova Game". */
  product: string;
  filename: string;
  sizeBytes: number;
  sizeHuman: string;
  /** Direct-file URL (GitHub asset browser_download_url). */
  downloadUrl: string;
}

/** The latest game release, parsed from the GitHub Releases API. */
export interface GameRelease {
  /** Tag with any leading "v" stripped, e.g. "0.0.9". */
  version: string;
  /** The GitHub release page (fallback link). */
  htmlUrl: string;
  assets: ReleaseAsset[];
}
