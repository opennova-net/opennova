// The products pages' shared words and lookups (pages/admin/ProductsSection.vue,
// the list, and ProductSection.vue, one product): a product's address (its
// project key, or its id when it has none), its kind in a line, which releases
// are its, and the release and tree states in words.

import type { Product, LobbyOption, Release } from '../types/products';

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

// The product's page: /admin/products/<key>, or /admin/products/<id> for a
// product with no project.
export function productLink(product: Product): string {
  return `/admin/products/${encodeURIComponent(product.key ?? String(product.id))}`;
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

// Every product: the list's rows, and the runtime when the list leaves it out
// and its releases name it (an app with no project, named after its section).
export function listProducts(products: readonly Product[], releases: readonly Release[]): Product[] {
  const out = [...products];
  for (const r of releases) {
    const id = releaseProductId(r);
    if (id === null || out.some((p) => p.id === id)) continue;
    const published = releases.find((p) => releaseProductId(p) === id && p.status === 'published');
    out.push({
      id,
      display_name: r.sections.find((s) => s.product_id === id)?.display_name || 'OpenNova runtime',
      kind: 'app',
      parent_id: null,
      expansion_code: '',
      title: '',
      lobby: '',
      updater_login: '',
      command_file: '',
      retail: false,
      key: null,
      visibility: null,
      head: 0,
      current_tree: null,
      published_release: published ? { id: published.id, name: published.name, sha256: published.sha256 } : null,
      servers_live: 0,
      servers_outdated: 0,
    });
  }
  return out;
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
// Operations", or the runtime's.
export function kindLine(product: Product, products: readonly Product[], lobbies: readonly LobbyOption[]): string {
  if (product.kind === 'app') return 'App: the OpenNova runtime, built by CI';
  if (product.kind === 'game') {
    const lobby = lobbies.find((l) => l.slug === product.lobby)?.display_name ?? product.lobby;
    return `Game ${titleName(product.title)}, lobby ${lobby}`;
  }
  const parent = products.find((p) => p.id === product.parent_id);
  return `Expansion ${product.expansion_code} of ${parent?.display_name ?? `game ${product.parent_id}`}`;
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
