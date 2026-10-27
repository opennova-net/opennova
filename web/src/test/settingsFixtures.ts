// The admin's settings as GET /api/admin/settings answers them (types/settings.ts).

import type { ServiceSettings } from '../types/settings';

export function serviceSettings(overrides: Partial<ServiceSettings> = {}): ServiceSettings {
  return {
    subscriptions_enabled: false,
    fleet: { max_shared: 2, shared_slots: 12, max_own: 2, max_addresses: 4 },
    boot: { max_shared: 2, shared_slots: 12, max_own: 2, max_addresses: 4 },
    addresses_available: true,
    capacity: {
      shared: { sold: 5, sellable: 24 },
      own_machines: { sold: 0, sellable: 2 },
      addresses: { sold: 0, sellable: 4 },
      oversold: false,
    },
    ...overrides,
  };
}
