// Shared axios instances. The plain `apiClient` is for public endpoints
// (`/api/lobbies`, `/api/stats`, `/api/register`, ...). The
// `adminClient` automatically attaches the bearer token from
// `authToken.ts` and clears it on a 401 so the next admin-page render
// re-prompts.

import axios, { AxiosError, type AxiosInstance } from 'axios';
import { clearAdminToken, getAdminToken } from './authToken';

const fallbackBaseUrl =
  typeof window !== 'undefined'
    ? '/api'
    : 'http://localhost:8080/api';

export const API_BASE_URL = import.meta.env.VITE_API_BASE_URL || fallbackBaseUrl;

// The server's CSRF header: every state-changing /api request a session
// cookie authenticates, and /api/register and /api/login, must carry it
// (apps/novaworld_server/README.md, "Website sessions and roles"). A
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

adminClient.interceptors.request.use((config) => {
  const token = getAdminToken();
  if (token) {
    config.headers = config.headers || {};
    config.headers.Authorization = `Bearer ${token}`;
  }
  return config;
});

adminClient.interceptors.response.use(
  (response) => response,
  (error: AxiosError) => {
    if (error.response?.status === 401) {
      clearAdminToken();
    }
    return Promise.reject(error);
  },
);
