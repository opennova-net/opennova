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

export const apiClient: AxiosInstance = axios.create({
  baseURL: API_BASE_URL,
});

export const adminClient: AxiosInstance = axios.create({
  baseURL: `${API_BASE_URL}/admin`,
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
