// Local-storage-backed admin bearer token for /api/admin/* requests: the
// maintainer's machine credential (the server's ADMIN_API_TOKEN), the
// fallback for when no admin-role session is signed in. Set on the /admin
// token-entry gate, attached to every admin axios call by the interceptor in
// `./client.ts`. Persisted across page reloads; cleared when the server
// refuses it (the interceptor wipes it, and `hasAdminToken` turning false
// makes the gate prompt again at once).

import { readonly, ref } from 'vue';

const KEY = 'opennova_admin_token';

function readStored(): string {
  if (typeof window === 'undefined') return '';
  try {
    return window.localStorage.getItem(KEY) || '';
  } catch {
    return '';
  }
}

const present = ref(Boolean(readStored()));

// Whether a token is stored, as reactive state for the admin gate.
export const hasAdminToken = readonly(present);

export function getAdminToken(): string {
  return readStored();
}

export function setAdminToken(token: string): void {
  if (typeof window !== 'undefined') {
    try {
      if (token) window.localStorage.setItem(KEY, token);
      else window.localStorage.removeItem(KEY);
    } catch {
      // Storage refused (private mode, blocked site data): no token is kept.
    }
  }
  present.value = Boolean(readStored());
}

export function clearAdminToken(): void {
  setAdminToken('');
}
