// Local-storage-backed admin bearer token for /api/admin/* requests.
// Set on /admin token-entry gate, attached to every admin axios call by
// the interceptor in `./client.ts`. Persisted across page reloads;
// cleared on 401 (the interceptor wipes it so the gate prompts again).

const KEY = 'opennova_admin_token';

export function getAdminToken(): string {
  if (typeof window === 'undefined') return '';
  return window.localStorage.getItem(KEY) || '';
}

export function setAdminToken(token: string): void {
  if (typeof window === 'undefined') return;
  if (token) window.localStorage.setItem(KEY, token);
  else        window.localStorage.removeItem(KEY);
}

export function clearAdminToken(): void {
  setAdminToken('');
}
