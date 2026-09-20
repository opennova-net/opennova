export interface DownloadManifest {
  platform: string;
  version: string;
  filename: string;
  downloadUrl: string;
  sizeHuman?: string;
  sizeBytes?: number;
  architecture?: string;
  description?: string;
  releaseNotes?: string;
}
