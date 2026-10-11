// Route access by session: a route whose record (or a parent's) carries
// `meta.requiresUser` opens only to a logged-in visitor; anyone else goes to
// /login?redirect=<the route>, which returns them after the login.

import type { LocationQueryValue, RouteLocationNormalized, RouteLocationRaw } from 'vue-router';
import { currentUser, loadSession } from '../composables/useSession';

declare module 'vue-router' {
  interface RouteMeta {
    // Only a logged-in visitor may open this route.
    requiresUser?: boolean;
  }
}

export async function requireUser(to: RouteLocationNormalized): Promise<true | RouteLocationRaw> {
  if (!to.matched.some((record) => record.meta.requiresUser)) return true;
  await loadSession();
  if (currentUser()) return true;
  return { path: '/login', query: { redirect: to.fullPath } };
}

// Where a login sends the visitor: the `redirect` query value when it is a
// path on this site, else the home page. Refused: anything not starting with
// one '/' ('//host' and '/\host' name another host), control bytes and
// backslashes (a browser strips or folds them into '//'), and /login itself.
export function safeRedirect(value: LocationQueryValue | LocationQueryValue[] | undefined): string {
  const raw = Array.isArray(value) ? value[0] : value;
  if (typeof raw !== 'string' || raw === '') return '/';
  if (!raw.startsWith('/') || raw.startsWith('//')) return '/';
  if (/[\u0000-\u001f\u007f\\]/.test(raw)) return '/';
  if (/^\/login(?:[/?#]|$)/.test(raw)) return '/';
  return raw;
}
