// The products pages' shared words and lookups (pages/admin/products and
// the account's projects): a product's address (its project key, or its id
// when it has none), its kind in a line, which releases are its, and the
// release and tree states in words.

import { titleName } from './games';
import { badgeClass } from './tone';
import type { Product, LobbyOption, Release } from '../types/products';

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

// The product's page: /admin/products/<key>.
export function productLink(product: Product): string {
  return `/admin/products/${encodeURIComponent(product.key)}`;
}

// The product an address names: by its project key first, then by its id.
export function findProduct(products: readonly Product[], ref: string): Product | null {
  const byKey = products.find((p) => p.key === ref);
  if (byKey) return byKey;
  return /^\d+$/.test(ref) ? (products.find((p) => p.id === Number(ref)) ?? null) : null;
}

// The product a release is of: its own, else its first section's;
// null for an upload not yet read.
export function releaseProductId(release: Release): number | null {
  return release.product_id ?? release.sections[0]?.product_id ?? null;
}

// A product's releases, newest first as the list has them; the runtime's also
// holds an upload the service has not read yet.
export function releasesOf(product: Product, releases: readonly Release[]): Release[] {
  return releases.filter((r) => {
    const id = releaseProductId(r);
    return id === product.id || (id === null && product.kind === 'app');
  });
}

// A release in a word: its version, else its name.
export function releaseLabel(release: Release): string {
  return release.version || release.name || `Release ${release.id}`;
}

// The version a product's published release brings, or '' with none.
export function publishedLabel(product: Product, releases: readonly Release[]): string {
  const published = product.published_release;
  if (!published) return '';
  const r = releases.find((x) => x.id === published.id);
  return r ? releaseLabel(r) : published.name || `Release ${published.id}`;
}

// "Game Joint Operations, lobby Joint Operations", "Expansion jox01 of Joint
// Operations", or "App, built by CI".
export function kindLine(product: Product, products: readonly Product[], lobbies: readonly LobbyOption[]): string {
  if (product.kind === 'app') return 'App, built by CI';
  if (product.kind === 'game') {
    const lobby = lobbies.find((l) => l.slug === product.lobby)?.display_name ?? product.lobby;
    return `Game ${titleName(product.title)}, lobby ${lobby}`;
  }
  const parent = products.find((p) => p.id === product.parent_id);
  return `Expansion ${product.expansion_code} of ${parent?.display_name ?? `game ${product.parent_id}`}`;
}

// Who started a release, from its created_by: the account's name ('user
// biggy', a player; 'admin root') after its words ("by", "by admin"), or the
// words alone for the admin token or the data program.
export function releaseCreator(createdBy: string): { words: string; account: string | null } {
  if (createdBy === 'token') return { words: 'by the admin token', account: null };
  if (createdBy === 'worker') return { words: 'by the data program', account: null };
  if (createdBy.startsWith('user ')) return { words: 'by', account: createdBy.slice(5) };
  if (createdBy.startsWith('admin ')) return { words: 'by admin', account: createdBy.slice(6) };
  return { words: createdBy, account: null };
}

// The same in a line: "by biggy", "by admin root", "by the admin token".
export function creatorLabel(createdBy: string): string {
  const { words, account } = releaseCreator(createdBy);
  return account === null ? words : `${words} ${account}`;
}

export function releaseStatusLabel(status: string): string {
  return (
    {
      uploading: 'Uploading',
      building: 'Building',
      validating: 'Checking',
      invalid: 'Invalid',
      draft: 'Draft',
      awaiting_approval: 'Awaiting approval',
      published: 'Published',
      retired: 'Retired',
    } as Record<string, string>
  )[status] ?? status;
}

export function releaseStatusClass(status: string): string {
  if (status === 'published') return badgeClass('ok');
  if (status === 'invalid') return badgeClass('bad');
  if (status === 'draft') return badgeClass('info');
  if (status === 'retired') return badgeClass('muted');
  return badgeClass('warn');
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

// A product's releases awaiting an admin's approval, newest first.
export function awaitingApproval(releases: readonly Release[]): Release[] {
  return releases.filter((r) => r.status === 'awaiting_approval');
}

// Something the data program is on (a release building or being checked, a
// tree building): the pages read again soon.
export function releasesWorking(releases: readonly Release[]): boolean {
  return releases.some(
    (r) => r.status === 'building' || r.status === 'validating' || r.status === 'uploading' || r.sections.some((s) => s.tree?.state === 'building'),
  );
}
