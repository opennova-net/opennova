export interface AdminExpansionFile {
  downloadUrl: string;
  sha256: string;
  sizeBytes?: number | null;
  fileType: string;
  orderIndex: number;
}

export interface AdminExpansion {
  id: number;
  gameId: number;
  slug: string;
  displayName: string;
  summary?: string | null;
  version: string;
  packageType: string;
  install: {
    subdir: string;
  };
  files: AdminExpansionFile[];
}

export type ExpansionReleaseStatus = 'pending' | 'tagged' | 'published' | 'failed';

export interface AdminRelease {
  id: number;
  slug: string;
  version: string;
  repoRef: string;
  status: ExpansionReleaseStatus;
  notes?: string | null;
  errorMessage?: string | null;
  workflowUrl?: string | null;
  targetCommit?: string | null;
  createdAt: string;
  updatedAt: string;
  publishedAt?: string | null;
}

export interface CreateReleaseRequest {
  version: string;
  repoRef?: string;
  notes?: string;
}

export interface CreateReleaseResponse {
  ok: boolean;
  message: string;
  release: AdminRelease;
  error?: string;
}

export interface AdminExpansionsResponse {
  expansions: AdminExpansion[];
}

export interface AdminReleasesResponse {
  releases: AdminRelease[];
}
