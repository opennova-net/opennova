export interface ExpansionFileSummary {
  downloadUrl: string;
  sha256: string;
  sizeBytes?: number;
  fileType: string;
}

export interface ExpansionGameSummary {
  slug: string;
  displayName: string;
}

export interface ExpansionSummary {
  slug: string;
  displayName: string;
  summary?: string | null;
  version: string;
  packageType?: string;
  featured: boolean;
  install?: {
    target: string;
  };
  releaseNotes?: string | null;
  gameSlug?: string | null;
  files?: ExpansionFileSummary[];
}

export interface ExpansionsResponse {
  expansions: ExpansionSummary[];
}
