// The admin's map: six sections in the sidebar, each a noun the admin looks
// after (players, servers, products, money, the system), and the tabs of the
// sections that hold more than one page. The sidebar, the crumbs and the
// Overview's links read it; the service's findings link to these paths
// (apps/novaworld_common/hosted/model/fleet_health.cpp).

import type { RouteLocationRaw } from 'vue-router';
import type { Tab } from '../components/ui/TabNav.vue';

export interface AdminSection {
  to: string;
  label: string;
  // The paths whose pages are this section's (a player's page is Players').
  prefixes: string[];
}

export const ADMIN_SECTIONS: readonly AdminSection[] = [
  { to: '/admin', label: 'Overview', prefixes: [] },
  { to: '/admin/players', label: 'Players', prefixes: ['/admin/players'] },
  { to: '/admin/servers', label: 'Servers', prefixes: ['/admin/servers', '/admin/machines'] },
  { to: '/admin/products', label: 'Products', prefixes: ['/admin/products'] },
  { to: '/admin/billing', label: 'Billing', prefixes: ['/admin/billing'] },
  { to: '/admin/system', label: 'System', prefixes: ['/admin/system', '/admin/jobs', '/admin/programs', '/admin/resources'] },
];

export const PLAYERS_TABS: Tab[] = [
  { label: 'Online', to: '/admin/players' },
  { label: 'Accounts', to: '/admin/players/accounts' },
  { label: 'CD keys', to: '/admin/players/cd-keys' },
];

export const SERVERS_TABS: Tab[] = [
  { label: 'Servers', to: '/admin/servers' },
  { label: 'Machines', to: '/admin/servers/machines' },
  { label: 'Roll', to: '/admin/servers/roll' },
];

export const BILLING_TABS: Tab[] = [
  { label: 'Subscriptions', to: '/admin/billing' },
  { label: 'Static addresses', to: '/admin/billing/addresses' },
  { label: 'AWS costs', to: '/admin/billing/costs' },
];

export const SYSTEM_TABS: Tab[] = [
  { label: 'NovaWorld', to: '/admin/system' },
  { label: 'Infrastructure', to: '/admin/system/infrastructure' },
  { label: 'Jobs', to: '/admin/system/jobs', pathOnly: true },
];

// A step of a breadcrumb: a link, or the page itself in words.
export interface Crumb {
  label: string;
  to?: RouteLocationRaw;
}

// Whether `path` is `prefix` or a page under it, never by a shared prefix
// alone (/admin/playersx is not /admin/players').
function under(path: string, prefix: string): boolean {
  return path === prefix || path.startsWith(`${prefix}/`);
}

// The section a path belongs to: the Overview for /admin itself, null for a
// path the admin does not know.
export function sectionOf(path: string): AdminSection | null {
  const clean = path.split(/[?#]/)[0].replace(/\/$/, '');
  if (clean === '/admin') return ADMIN_SECTIONS[0];
  return ADMIN_SECTIONS.find((s) => s.prefixes.some((p) => under(clean, p))) ?? null;
}
