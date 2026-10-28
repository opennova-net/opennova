// The player's own pages: four sections down the side, and the tabs of the
// one that holds more than one page. AccountLayout, the pages' crumbs and the
// Overview's tiles read it.

import type { Tab } from '../components/ui/TabNav.vue';

export interface AccountSection {
  to: string;
  label: string;
}

export const ACCOUNT_SECTIONS: readonly AccountSection[] = [
  { to: '/me', label: 'Overview' },
  { to: '/me/servers', label: 'Servers' },
  { to: '/me/projects', label: 'Projects' },
  { to: '/me/billing', label: 'Billing' },
];

export const MY_SERVERS_TABS: Tab[] = [
  { label: 'Servers', to: '/me/servers' },
  { label: 'Stats', to: '/me/servers/stats' },
];

// The section a path belongs to (a server's page is Servers', a project's
// Projects'), never by a shared prefix alone; null for none.
export function accountSectionOf(path: string): AccountSection | null {
  const clean = path.split(/[?#]/)[0].replace(/\/$/, '');
  if (clean === '/me') return ACCOUNT_SECTIONS[0];
  return ACCOUNT_SECTIONS.slice(1).find((s) => clean === s.to || clean.startsWith(`${s.to}/`)) ?? null;
}
