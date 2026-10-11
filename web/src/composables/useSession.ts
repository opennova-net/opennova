// The signed-in visitor, app-wide: one reactive current user, read from
// GET /api/me at app start (main.ts calls loadSession) and again after a
// login, a logout, or an admin request the server refused (an expired
// session, or a role changed under a live one). A 401 from /api/me means
// logged out.

import { computed, ref, shallowRef } from 'vue';
import { clearAdminToken } from '../api/authToken';
import { onAdminCredentialRefused } from '../api/client';
import {
  login,
  logout,
  me,
  sessionError,
  type SessionError,
  type SessionResult,
  type SessionUser,
} from '../api/session';

const user = shallowRef<SessionUser | null>(null);
// The first /api/me has answered (or failed): until then the user is unknown,
// not logged out.
const ready = ref(false);
// The last /api/me failure other than a 401 (the server unreachable, say).
const loadError = shallowRef<SessionError | null>(null);

// The load every caller of loadSession shares: set while one is in flight or
// once one has answered; null again after a failed one, so the next caller
// asks again.
let initialLoad: Promise<void> | null = null;
// Each refresh's ticket: only the latest one writes the state, so a slow
// /api/me cannot overwrite a newer answer.
let latest = 0;

// Re-reads GET /api/me. On a failure other than a 401 the last known user is
// kept and the failure recorded.
export async function refreshSession(): Promise<SessionResult<SessionUser | null>> {
  const ticket = ++latest;
  const result = await me();
  if (ticket === latest) {
    if (result.ok) {
      user.value = result.value;
      loadError.value = null;
      // An answer from any refresh settles the load.
      if (!initialLoad) initialLoad = Promise.resolve();
    } else {
      loadError.value = result.error;
    }
    ready.value = true;
  }
  return result;
}

// The app-start load; later callers (the router guard, on every navigation)
// share it. A load that got no answer (the server unreachable, a 5xx, the
// request timing out) is not kept: `ready` still turns true, showing the
// visitor as logged out, and the next caller tries again.
export function loadSession(): Promise<void> {
  if (!initialLoad) {
    const attempt: Promise<void> = refreshSession().then((result) => {
      if (!result.ok && initialLoad === attempt) initialLoad = null;
    });
    initialLoad = attempt;
  }
  return initialLoad;
}

// The current user, without waiting: null while logged out or still loading.
export function currentUser(): SessionUser | null {
  return user.value;
}

// POST /api/login, then /api/me to confirm the browser kept the cookie: a
// login the server accepted whose cookie the browser dropped (a Secure cookie
// on plain http) would otherwise show a user the next request does not have.
export async function signIn(
  username: string,
  password: string,
): Promise<SessionResult<SessionUser>> {
  const result = await login(username, password);
  if (!result.ok) return result;
  const check = await refreshSession();
  if (check.ok) {
    if (check.value === null) return { ok: false, error: sessionError('cookie_not_kept', 200) };
    return { ok: true, value: check.value };
  }
  // /api/me did not answer: trust the login's own reply.
  latest++;
  user.value = result.value;
  ready.value = true;
  return result;
}

// POST /api/logout, then /api/me. The user is cleared as soon as the server
// confirms the logout. A stored admin token is forgotten too, first and
// whatever the server answers, so a shared browser keeps no admin access
// after a Log out.
export async function signOut(): Promise<SessionResult<void>> {
  clearAdminToken();
  const result = await logout();
  if (result.ok) {
    latest++;
    user.value = null;
    ready.value = true;
  }
  await refreshSession();
  return result;
}

onAdminCredentialRefused(() => {
  void refreshSession();
});

export function useSession() {
  return {
    user: computed(() => user.value),
    ready: computed(() => ready.value),
    loadError: computed(() => loadError.value),
    isLoggedIn: computed(() => user.value !== null),
    isAdmin: computed(() => user.value?.role === 'admin'),
    signIn,
    signOut,
    refresh: refreshSession,
  };
}
