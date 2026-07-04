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

/** Operating system a tool build targets. 'any' = cross-platform (e.g. Blender add-on). */
export type ToolOs = 'windows' | 'macos' | 'any';

/** A single downloadable deliverable from a GitHub Release. */
export interface ToolAsset {
  /** Human product name, e.g. "OpenNova Editor (ONED)". */
  product: string;
  os: ToolOs;
  /** Broad grouping used for ordering: apps first, then tools/plugins. */
  kind: 'app' | 'tool' | 'plugin';
  filename: string;
  sizeBytes: number;
  sizeHuman: string;
  /** Direct-file URL (GitHub asset browser_download_url). */
  downloadUrl: string;
}

/** The latest tool release, parsed from the GitHub Releases API. */
export interface ToolRelease {
  /** Tag with any leading "v" stripped, e.g. "0.0.9". */
  version: string;
  /** The GitHub release page (fallback link). */
  htmlUrl: string;
  assets: ToolAsset[];
}
