export interface AdminExpansionFile {
  downloadUrl: string;
  sha256: string;
  sizeBytes?: number | null;
  fileType: string;
  orderIndex: number;
}

export interface AdminExpansion {
  id: number;
  gameSlug: string;
  slug: string;
  displayName: string;
  summary?: string | null;
  version: string;
  packageType: string;
  featured: boolean;
  githubRepo?: string | null;
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

// Server status / maintenance. Matches GET/PUT /api/admin/server-status, which
// emits snake_case (apps/novaworld_server/http_listener.cpp get/update_server_status).
export interface ServerStatus {
  maintenance_enabled: boolean;
  message: string;
}

// A live connection from GET /api/admin/connections (snake_case, straight from
// the in-memory connection registry snapshot).
export interface AdminConnection {
  id: number;
  state: string;
  pn: number;
  identity: string;
  created_ms: number;
  last_seen_ms: number;
  addr: string;
}

export interface ConnectionsResponse {
  count: number;
  heartbeat_timeout_ms: number;
  connections: AdminConnection[];
}
