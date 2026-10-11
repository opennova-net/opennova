// Shared axios instances. The plain `apiClient` is for public endpoints
// (`/api/lobbies`, `/api/stats`, `/api/register`, ...) and the website
// session (`/api/login`, `/api/logout`, `/api/me`, `./session.ts`). The
// `adminClient` is for /api/admin/*, which the server opens to an admin-role
// session or to the Bearer ADMIN_API_TOKEN: it attaches the bearer token from
// `authToken.ts` when one is stored, and clears it when the server refuses it.
//
// Cookies: the session cookie is HttpOnly and set by the server. The site
// calls the API on its own origin (`/api`, through nginx in prod and Vite's
// dev proxy in dev), and a browser sends cookies on same-origin requests
// whatever `withCredentials` says (it only governs cross-origin ones), so
// neither instance sets it. The server grants no CORS, so a cross-origin
// VITE_API_BASE_URL could not carry a session anyway.

import axios, { AxiosError, type AxiosInstance } from 'axios';
import { clearAdminToken, getAdminToken } from './authToken';

const fallbackBaseUrl =
  typeof window !== 'undefined'
    ? '/api'
    : 'http://localhost:8080/api';

export const API_BASE_URL = import.meta.env.VITE_API_BASE_URL || fallbackBaseUrl;

// The server's CSRF header: every state-changing /api request a session
// cookie authenticates, and /api/register, /api/login and /api/logout, must
// carry it (apps/novaworld_server/README.md, "Website sessions and roles"). A
// cross-site page cannot send it, so it marks a request as the site's own.
const CSRF_HEADERS = { 'X-OpenNova-Request': '1' };

export const apiClient: AxiosInstance = axios.create({
  baseURL: API_BASE_URL,
  headers: CSRF_HEADERS,
});

export const adminClient: AxiosInstance = axios.create({
  baseURL: `${API_BASE_URL}/admin`,
  headers: CSRF_HEADERS,
});

function bearer(token: string): string {
  return `Bearer ${token}`;
}

// Only a stored, non-blank token is sent; with none the request carries no
// Authorization header and the session cookie alone speaks for it. (The
// server would fall back to the session for a wrong or empty Bearer too, but
// a header that cannot authorize has no business on the wire.)
adminClient.interceptors.request.use((config) => {
  const token = getAdminToken().trim();
  if (token) {
    config.headers.Authorization = bearer(token);
  }
  return config;
});

// Told when the server refuses an admin request's credentials, so the
// session state can be re-read (an expired session, or a role an admin
// changed since the page loaded).
const credentialRefusedListeners = new Set<() => void>();

export function onAdminCredentialRefused(listener: () => void): () => void {
  credentialRefusedListeners.add(listener);
  return () => credentialRefusedListeners.delete(listener);
}

// 401: neither a Bearer token nor a session was accepted. 403 `forbidden`: a
// player's session and no accepted token. Either way a token the request
// carried was refused, so it is dropped (only if it is still the stored one:
// a token entered while the request was in flight stays).
adminClient.interceptors.response.use(
  (response) => response,
  (error: AxiosError) => {
    const status = error.response?.status;
    const code = (error.response?.data as { error?: unknown } | undefined)?.error;
    if (status === 401 || (status === 403 && code === 'forbidden')) {
      const sent = error.config?.headers?.Authorization;
      const stored = getAdminToken().trim();
      if (stored && sent === bearer(stored)) {
        clearAdminToken();
      }
      credentialRefusedListeners.forEach((listener) => listener());
    }
    return Promise.reject(error);
  },
);
