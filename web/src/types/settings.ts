import type { SoldCapacity } from './users';

// The fleet's four maximums (hosted/model/placement.h PlacementPolicy).
export interface FleetLimits {
  // Shared machines at most, and the slots a new one has.
  max_shared: number;
  shared_slots: number;
  // Dedicated machines at most.
  max_own: number;
  // Static addresses at most (0 where the deployment runs none).
  max_addresses: number;
}

// The admin's settings (GET /api/admin/settings, settings_routes.h).
export interface ServiceSettings {
  // Whether support tiers, dedicated machines and static addresses sell.
  subscriptions_enabled: boolean;
  // What the fleet runs by now.
  fleet: FleetLimits;
  // The deployment's own (ONNET_FLEET_*), which a maximum with no setting takes.
  boot: FleetLimits;
  // The deployment runs static addresses: max_addresses may be set.
  addresses_available: boolean;
  // The service's sold capacity against `fleet`; null when the reply named none.
  capacity: SoldCapacity | null;
}

// What PUT /api/admin/settings takes: every key, each maximum a whole number.
export interface ServiceSettingsChange {
  subscriptions_enabled: boolean;
  max_shared: number;
  shared_slots: number;
  max_own: number;
  max_addresses: number;
}

// The highest a machine or address maximum may be set to, and a shared
// machine's slots (service_settings.h kMaxFleetMachines, placement.h kMaxSlots).
export const FLEET_MACHINES_MAX = 64;
export const SHARED_SLOTS_MAX = 32;
