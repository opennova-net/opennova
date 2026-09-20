// One axios error -> display string for the admin sections. The server's JSON
// error bodies carry `message` and/or `error`; `prefer` picks which one wins
// when both are present.

import { isAxiosError } from 'axios';

export function extractError(err: unknown, prefer: 'message' | 'error' = 'message'): string {
  if (isAxiosError(err)) {
    const data = err.response?.data as Record<string, any> | undefined;
    const other = prefer === 'message' ? 'error' : 'message';
    if (typeof data?.[prefer] === 'string') return data[prefer];
    if (typeof data?.[other] === 'string') return data[other];
    return err.response?.statusText || 'Request failed';
  }
  if (err instanceof Error) return err.message;
  return 'Unexpected error';
}
