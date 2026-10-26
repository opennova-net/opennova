// The games pages' shared words and lookups (pages/admin/GamesSection.vue, the
// list, and GameSection.vue, one game): a game's address (its project key, or
// its id when it has none), its kind in a line, which releases are its, and
// the release and tree states in words.

import type { DataSet, LobbyOption, Release } from '../types/dataSets';

const TITLE_NAMES: Record<string, string> = { jo: 'Joint Operations', dfx2: 'Delta Force Xtreme 2' };

// A project key: 1 to 16 of A-Z, 0-9 and _ (the routes' rule).
export const KEY_RULE = /^[A-Z0-9_]{1,16}$/;
// A release version: one to four numbers between dots (0.2.0, 1.7.5.9).
export const VERSION_RULE = /^[0-9]+(\.[0-9]+){0,3}$/;

// The updater's login: 1 to 32 printable ASCII characters without spaces;
// the command file: 1 to 15, ending in .CMD. Both or neither.
const UPDATER_LOGIN = /^[\x21-\x7e]{1,32}$/;
const COMMAND_FILE = /^[\x21-\x7e]{5,15}$/;

// The updater fields' problems: both or neither, each in its shape.
export function updaterProblems(login: string, commandFile: string): { updater_login?: string; command_file?: string } {
  const out: { updater_login?: string; command_file?: string } = {};
  if (login === '' && commandFile === '') return out;
  if (login === '') out.updater_login = 'A command file needs the updater login that goes with it.';
  else if (!UPDATER_LOGIN.test(login)) out.updater_login = 'An updater login is 1 to 32 printable ASCII characters without spaces.';
  if (commandFile === '') out.command_file = 'An updater login needs its command file.';
  else if (!COMMAND_FILE.test(commandFile) || !commandFile.endsWith('.CMD')) {
    out.command_file = 'A command file is 1 to 15 characters ending in .CMD (JOPTR.CMD).';
  }
  return out;
}

export function titleName(code: string): string {
  return TITLE_NAMES[code] ?? code.toUpperCase();
}

// The game's page: /admin/games/<key>, or /admin/games/<id> for a game with
// no project.
export function gameLink(game: DataSet): string {
  return `/admin/games/${encodeURIComponent(game.key ?? String(game.id))}`;
}

// The game an address names: by its project key first, then by its id.
export function findGame(games: readonly DataSet[], ref: string): DataSet | null {
  const byKey = games.find((g) => g.key === ref);
  if (byKey) return byKey;
  return /^\d+$/.test(ref) ? (games.find((g) => g.id === Number(ref)) ?? null) : null;
}

// The game a release is of: its own, else its first section's;
// null for an upload not yet read.
export function releaseGameId(release: Release): number | null {
  return release.data_set_id ?? release.sections[0]?.data_set_id ?? null;
}

// Every game: the list's rows, and the runtime when the list leaves it out
// and its releases name it (an update game with no project, named after its
// section).
export function listGames(dataSets: readonly DataSet[], releases: readonly Release[]): DataSet[] {
  const games = [...dataSets];
  for (const r of releases) {
    const id = releaseGameId(r);
    if (id === null || games.some((g) => g.id === id)) continue;
    const published = releases.find((p) => releaseGameId(p) === id && p.status === 'published');
    games.push({
      id,
      display_name: r.sections.find((s) => s.data_set_id === id)?.display_name || 'OpenNova runtime',
      kind: 'update',
      parent_id: null,
      expansion_code: '',
      title: '',
      game: '',
      updater_login: '',
      command_file: '',
      retail_updater: false,
      key: null,
      visibility: null,
      head: 0,
      current_tree: null,
      published_release: published ? { id: published.id, name: published.name, sha256: published.sha256 } : null,
      servers_live: 0,
      servers_outdated: 0,
    });
  }
  return games;
}

// A game's releases, newest first as the list has them; the runtime's also
// holds an upload the service has not read yet.
export function releasesOf(game: DataSet, releases: readonly Release[]): Release[] {
  return releases.filter((r) => {
    const id = releaseGameId(r);
    return id === game.id || (id === null && game.kind === 'update');
  });
}

// A release in a word: its version, else its name.
export function releaseLabel(release: Release): string {
  return release.version || release.name || `Release ${release.id}`;
}

// The version a game's published release brings, or '' with none.
export function publishedLabel(game: DataSet, releases: readonly Release[]): string {
  const published = game.published_release;
  if (!published) return '';
  const r = releases.find((x) => x.id === published.id);
  return r ? releaseLabel(r) : published.name || `Release ${published.id}`;
}

// "Base game Joint Operations, lobby Joint Operations", "Expansion jox01 of
// Joint Operations", or the runtime's.
export function kindLine(game: DataSet, games: readonly DataSet[], lobbies: readonly LobbyOption[]): string {
  if (game.kind === 'update') return 'The OpenNova runtime, built by CI';
  if (game.kind === 'base') {
    const lobby = lobbies.find((l) => l.slug === game.game)?.display_name ?? game.game;
    return `Base game ${titleName(game.title)}, lobby ${lobby}`;
  }
  const parent = games.find((p) => p.id === game.parent_id);
  return `Expansion ${game.expansion_code} of ${parent?.display_name ?? `game ${game.parent_id}`}`;
}

export function creatorLabel(createdBy: string): string {
  if (createdBy === 'token') return 'by the admin token';
  if (createdBy === 'worker') return 'by the data program';
  return createdBy.startsWith('admin ') ? `by ${createdBy.slice(6)}` : createdBy;
}

export function releaseStatusLabel(status: string): string {
  return (
    {
      uploading: 'Uploading',
      building: 'Building',
      validating: 'Checking',
      invalid: 'Invalid',
      draft: 'Draft',
      published: 'Published',
      retired: 'Retired',
    } as Record<string, string>
  )[status] ?? status;
}

const BADGE = 'rounded-control px-2 py-0.5 text-xs font-medium';

export function releaseStatusClass(status: string): string {
  if (status === 'published') return `${BADGE} bg-online/10 text-online`;
  if (status === 'invalid') return `${BADGE} bg-danger/10 text-danger`;
  if (status === 'draft') return `${BADGE} bg-accent/15 text-accent`;
  if (status === 'retired') return `${BADGE} bg-raised text-ink-muted`;
  return `${BADGE} bg-warn/10 text-warn`;
}

// A published release's tree for servers, in words: "Servers' data building".
export function treeStateText(state: string): string {
  return (
    { building: "Servers' data building", ready: "Servers' data ready", failed: "Servers' data failed", pruned: "Servers' data pruned" } as Record<
      string,
      string
    >
  )[state] ?? state;
}

// Something the data program is on (a release building or being checked, a
// tree building): the pages read again soon.
export function releasesWorking(releases: readonly Release[]): boolean {
  return releases.some(
    (r) => r.status === 'building' || r.status === 'validating' || r.status === 'uploading' || r.sections.some((s) => s.tree?.state === 'building'),
  );
}
