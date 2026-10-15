import { AxiosError, AxiosHeaders } from 'axios';
import { describe, expect, it } from 'vitest';
import { describeStatsError } from './hostedStats';

function refusal(status: number, data: unknown): AxiosError {
  const config = { headers: new AxiosHeaders() };
  return new AxiosError('refused', 'ERR_BAD_REQUEST', config, null, {
    status,
    statusText: '',
    headers: {},
    config,
    data,
  });
}

describe('describeStatsError', () => {
  it("maps the stats routes' range refusal (invalid_field) to its message", () => {
    const error = describeStatsError(refusal(400, { error: 'invalid_field', message: 'raw server text' }));
    expect(error.code).toBe('invalid_field');
    expect(error.message).toContain('could not read the request');
    expect(error.message).not.toContain('raw server text');
  });
});
