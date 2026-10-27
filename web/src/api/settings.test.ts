import { describe, expect, it, vi } from 'vitest';
import { adminClient } from './client';
import { fetchSettings, parseSettings, saveSettings } from './settings';
import { serviceSettings } from '../test/settingsFixtures';

vi.mock('./client', () => ({
  apiClient: { get: vi.fn(), post: vi.fn(), put: vi.fn() },
  adminClient: { get: vi.fn(), post: vi.fn(), put: vi.fn() },
}));

const get = vi.mocked(adminClient.get);
const put = vi.mocked(adminClient.put);

const wire = (value: unknown): unknown => JSON.parse(JSON.stringify(value));

describe('the settings client', () => {
  it('reads the settings as the server sends them', async () => {
    const settings = serviceSettings();
    expect(parseSettings(wire(settings))).toEqual(settings);
    get.mockResolvedValue({ status: 200, data: wire(settings) });
    expect(await fetchSettings()).toEqual({ ok: true, value: settings });
    expect(get).toHaveBeenCalledWith('/settings', { signal: undefined });
  });

  it('refuses a reply without the switch or a whole fleet', () => {
    const settings = serviceSettings();
    const { subscriptions_enabled: _drop, ...unsaid } = settings;
    expect(parseSettings(wire(unsaid))).toBeNull();
    expect(parseSettings(wire({ ...settings, fleet: { ...settings.fleet, max_shared: -1 } }))).toBeNull();
    expect(parseSettings(wire({ ...settings, boot: null }))).toBeNull();
    expect(parseSettings(wire({ ...settings, capacity: undefined }))?.capacity).toBeNull();
  });

  it('saves every setting at once', async () => {
    const saved = serviceSettings({ subscriptions_enabled: true });
    put.mockResolvedValue({ status: 200, data: wire(saved) });
    const change = { subscriptions_enabled: true, max_shared: 4, shared_slots: 12, max_own: 2, max_addresses: 4 };
    expect(await saveSettings(change)).toEqual({ ok: true, value: saved });
    expect(put).toHaveBeenCalledWith('/settings', change);
  });
});
