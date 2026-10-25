// The content store, as its routes answer it (snake_case; apps/novaworld_web/content/content_routes.h,
// ADR 0069): each project is an OpenNova Editor project's files, kept by revision. Editors push and
// pull them; the admin's page manages the projects and reads their history. Times are UTC
// 'YYYY-MM-DD HH:MM:SS'.

export type ContentVisibility = 'public' | 'private';

export interface ContentStorage {
  // The files the store holds for it (every revision's, each once), and their bytes.
  files: number;
  bytes: number;
}

export interface ContentProject {
  // 1 to 16 of A-Z, 0-9 and _ ('OPENNOVA', 'JO').
  key: string;
  title: string;
  visibility: ContentVisibility;
  // A retail game's import: its revision 1 is the stock files, so a release is what changed since.
  retail: boolean;
  // The project an expansion's builds on, by key; null for a base game.
  base: string | null;
  // The data set its releases update, or null.
  data_set_id: number | null;
  // Its newest revision; 0 before the first push.
  head: number;
  can_push: boolean;
  // An admin's read carries it.
  storage: ContentStorage | null;
}

export interface ContentRevision {
  number: number;
  // Who pushed it.
  author: string;
  message: string;
  manifest_sha256: string;
  file_count: number;
  bytes: number;
  created_at: string;
}

// A new project, as an admin makes it.
export interface NewContentProject {
  key: string;
  title: string;
  visibility: ContentVisibility;
  retail: boolean;
  data_set_id: number | null;
  base: string | null;
}
