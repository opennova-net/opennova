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
