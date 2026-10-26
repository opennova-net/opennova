// One navigation map for the sidebar, breadcrumbs and Overview. Flat paths
// remain shared with backend findings and operator tools.

import type { RouteLocationRaw } from 'vue-router';

export interface AdminSection {
  to: string;
  label: string;
  // What it holds, in a line: the Overview's links say it.
  description: string;
}

export interface AdminArea {
  id: 'service' | 'accounts' | 'operations';
  label: string;
  description: string;
  sections: AdminSection[];
}

export const OVERVIEW: AdminSection = {
  to: '/admin/overview',
  label: 'Overview',
  description: 'What needs you, and every area at a glance',
};

export const ADMIN_AREAS: readonly AdminArea[] = [
  {
    id: 'service',
    label: 'Service',
    description: 'NovaWorld, game servers and their releases',
    sections: [
      { to: '/admin/novaworld', label: 'NovaWorld', description: 'Sign-in, live sessions and lobby instances' },
      { to: '/admin/fleet', label: 'Fleet', description: 'Machines and servers, their moves, the roll' },
      { to: '/admin/games', label: 'Games', description: "Each game's project, its releases and the servers running it" },
      { to: '/admin/addresses', label: 'Static addresses', description: "Dedicated machines' fixed addresses and names" },
    ],
  },
  {
    id: 'accounts',
    label: 'Accounts',
    description: 'Players, subscriptions and access',
    sections: [
      { to: '/admin/users', label: 'Users', description: 'Accounts, their roles, tiers and servers' },
      { to: '/admin/billing', label: 'Subscriptions', description: 'Support tiers, dedicated machines and addresses, as Stripe has them' },
      { to: '/admin/cd-keys', label: 'CD keys', description: 'The Delta Force Xtreme 2 keys a copy activates' },
    ],
  },
  {
    id: 'operations',
    label: 'Operations',
    description: 'The deployment as it runs',
    sections: [
      { to: '/admin/infrastructure', label: 'Infrastructure', description: 'Programs, tasks, the balancer, the database and cache' },
      { to: '/admin/usage', label: 'Server usage', description: 'What every live server uses of its slot' },
      { to: '/admin/costs', label: 'AWS costs', description: "The month's AWS bill and account budgets" },
      { to: '/admin/jobs', label: 'Background jobs', description: 'The queue, its failures and its schedules' },
    ],
  },
];

// A step of a breadcrumb past a section's page (AdminCrumbs' trail): a link,
// or the page itself in words.
export interface Crumb {
  label: string;
  to?: RouteLocationRaw;
}

// Whether `path` is the section's page or a page under it (a user's, a fleet
// server's, a job's).
export function isUnder(path: string, section: AdminSection): boolean {
  return path === section.to || path.startsWith(`${section.to}/`);
}

// The area and section a path belongs to; the Overview's has no area. Null for
// a path the admin does not know.
export function placeOf(path: string): { area: AdminArea | null; section: AdminSection } | null {
  if (isUnder(path, OVERVIEW)) return { area: null, section: OVERVIEW };
  for (const area of ADMIN_AREAS) {
    const section = area.sections.find((s) => isUnder(path, s));
    if (section) return { area, section };
  }
  return null;
}
