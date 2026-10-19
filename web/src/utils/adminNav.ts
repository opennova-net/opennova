// The admin's map: the Overview, then four areas the way an operator thinks of
// the service, each with its pages. The admin menu (pages/admin/AdminLayout.vue),
// every page's breadcrumb (components/AdminCrumbs.vue) and the Overview's links
// into each area read it, so a page has one place in all three.
//
//   NovaWorld         the service retail players use: whether they can sign in,
//                     who is signed in, the CD keys DFX2 activates
//   Hosting           the hosted servers: the fleet's machines and servers, what
//                     they use, their static addresses, the game data they run
//   People and money  the accounts, their subscriptions, and what AWS charges
//   Operations        the deployment as it runs: its programs and AWS resources,
//                     and the background jobs
//
// Paths stay flat (/admin/fleet, /admin/users/<id>): the attention list's links
// (apps/novaworld_common/hosted/model/fleet_health.cpp), the toolbox's output and
// the docs name them.

import type { RouteLocationRaw } from 'vue-router';

export interface AdminSection {
  to: string;
  label: string;
  // What it holds, in a line: the Overview's links say it.
  description: string;
}

export interface AdminArea {
  id: 'novaworld' | 'hosting' | 'people' | 'operations';
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
    id: 'novaworld',
    label: 'NovaWorld',
    description: 'The service retail players sign in to',
    sections: [
      { to: '/admin/novaworld', label: 'Status and sessions', description: 'Whether players can sign in, and who is signed in now' },
      { to: '/admin/cd-keys', label: 'CD keys', description: 'The Delta Force Xtreme 2 keys a copy activates' },
    ],
  },
  {
    id: 'hosting',
    label: 'Hosting',
    description: 'The hosted game servers and the machines they run on',
    sections: [
      { to: '/admin/fleet', label: 'Fleet', description: 'Machines and servers, their moves, the roll' },
      { to: '/admin/usage', label: 'Server usage', description: 'What every live server uses of its slot' },
      { to: '/admin/addresses', label: 'Static addresses', description: "Own machines' fixed addresses and names" },
      { to: '/admin/data-sets', label: 'Game data', description: 'The data sets servers run on, and their updates' },
    ],
  },
  {
    id: 'people',
    label: 'People and money',
    description: 'Accounts, what they pay, and what AWS charges',
    sections: [
      { to: '/admin/users', label: 'Users', description: 'Accounts, their roles, tiers and servers' },
      { to: '/admin/billing', label: 'Subscriptions', description: 'Support tiers and add-ons, as Stripe has them' },
      { to: '/admin/costs', label: 'AWS costs', description: "The month's AWS bill against the budget" },
    ],
  },
  {
    id: 'operations',
    label: 'Operations',
    description: 'The deployment as it runs',
    sections: [
      { to: '/admin/infrastructure', label: 'Infrastructure', description: 'Programs, tasks, the balancer, the database and cache' },
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
