// The admin's live settings (apps/novaworld_web/settings_routes.h): GET and
// PUT /api/admin/settings, whether subscriptions sell and the fleet's
// maximums (the Machines tab's Capacity). Each answers an AdminResult
// (admin.ts).

import { adminClient } from './client';
import { adminCall, type AdminResult } from './admin';
import { pickCapacity } from './users';
import type { FleetLimits, ServiceSettings, ServiceSettingsChange } from '../types/settings';

const isCount = (v: unknown): v is number => Number.isInteger(v) && (v as number) >= 0;

function pickLimits(value: unknown): FleetLimits | null {
  const v = value as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null) return null;
  const { max_shared, shared_slots, max_own, max_addresses } = v;
  if (!isCount(max_shared) || !isCount(shared_slots) || !isCount(max_own) || !isCount(max_addresses)) return null;
  return { max_shared, shared_slots, max_own, max_addresses };
}

export function parseSettings(data: unknown): ServiceSettings | null {
  const v = data as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null || typeof v.subscriptions_enabled !== 'boolean') return null;
  const fleet = pickLimits(v.fleet);
  const boot = pickLimits(v.boot);
  if (!fleet || !boot) return null;
  return {
    subscriptions_enabled: v.subscriptions_enabled,
    fleet,
    boot,
    addresses_available: v.addresses_available === true,
    capacity: pickCapacity(v.capacity),
  };
}

// GET /api/admin/settings.
export function fetchSettings(signal?: AbortSignal): Promise<AdminResult<ServiceSettings>> {
  return adminCall(() => adminClient.get<unknown>('/settings', { signal }), parseSettings);
}

// PUT /api/admin/settings: every setting at once; the settings as saved.
export function saveSettings(change: ServiceSettingsChange): Promise<AdminResult<ServiceSettings>> {
  return adminCall(() => adminClient.put<unknown>('/settings', change), parseSettings);
}
