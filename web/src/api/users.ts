// Admin user management (apps/novaworld_server/admin_user_routes.cpp), through
// `adminClient`: an admin-role session's cookie authorizes it, or the bearer
// token from local storage, which the client attaches when one is stored and
// drops when refused. Each call answers an AdminResult (admin.ts).
//
// Public self-signup at /api/register goes through the plain `apiClient`, so
// anonymous browsers can call it; it throws as axios does (RegisterPage).

import { adminClient, apiClient } from './client';
import { adminCall, type AdminResult } from './admin';
import { isHostedServer } from './hostedServer';
import type { AdminSubscription, TierNumber } from '../types/billing';
import type { EntitlementSource } from '../types/hostedServer';
import type {
  CreateUserPayload,
  RegisterPayload,
  Role,
  UpdateUserPayload,
  User,
  UserDetail,
  UserEntitlementSummary,
  UserListEntry,
  UserResponse,
} from '../types/users';

const str = (v: unknown): string => (typeof v === 'string' ? v : '');
const count = (v: unknown): number => (Number.isInteger(v) && (v as number) >= 0 ? (v as number) : 0);
const tierOf = (v: unknown): TierNumber | null => (v === 1 || v === 2 || v === 3 ? v : null);
const SOURCES: ReadonlySet<unknown> = new Set<EntitlementSource>(['none', 'tier', 'comp']);
const sourceOf = (v: unknown): EntitlementSource => (SOURCES.has(v) ? (v as EntitlementSource) : 'none');

// An account, or null when it has no id, name or role.
export function pickUser(value: unknown): User | null {
  const v = value as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null || typeof v.id !== 'number' || typeof v.username !== 'string') return null;
  if (v.role !== 'player' && v.role !== 'admin') return null;
  return {
    id: v.id,
    username: v.username,
    pcid: str(v.pcid),
    nwh: str(v.nwh),
    nwhandle: str(v.nwhandle),
    account_status: str(v.account_status),
    role: v.role as Role,
    comp_servers: count(v.comp_servers),
    max_servers: count(v.max_servers ?? (v.entitlement as { max_servers?: unknown } | undefined)?.max_servers),
  };
}

function pickEntitlement(value: unknown, user: User): UserEntitlementSummary {
  const v = (value ?? {}) as Record<string, unknown>;
  return {
    max_servers: Number.isInteger(v.max_servers) ? (v.max_servers as number) : user.max_servers,
    source: sourceOf(v.source),
    tier: tierOf(v.tier),
  };
}

function pickListEntry(value: unknown): UserListEntry | null {
  const user = pickUser(value);
  if (!user) return null;
  const v = value as Record<string, unknown>;
  return {
    ...user,
    entitlement: pickEntitlement(v.entitlement, user),
    live_servers: count(v.live_servers),
    subscription_status: typeof v.subscription_status === 'string' ? v.subscription_status : null,
  };
}

export function pickSubscription(value: unknown): AdminSubscription | null {
  const v = value as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null || typeof v.stripe_subscription_id !== 'string') return null;
  return {
    stripe_subscription_id: v.stripe_subscription_id,
    item: v.item === 'own_machine' ? 'own_machine' : 'tier',
    tier: tierOf(v.tier),
    quantity: count(v.quantity) || 1,
    status: str(v.status),
    entitled: v.entitled === true,
    current_period_end: str(v.current_period_end),
    cancel_at_period_end: v.cancel_at_period_end === true,
  };
}

function pickDetail(data: unknown): UserDetail | null {
  const v = data as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null) return null;
  const user = pickUser(v.user);
  if (!user) return null;
  const e = (v.entitlement ?? {}) as Record<string, unknown>;
  const servers = Array.isArray(v.servers) ? v.servers.filter(isHostedServer) : [];
  const billing = (v.billing ?? {}) as Record<string, unknown>;
  const subscriptions = Array.isArray(billing.subscriptions)
    ? billing.subscriptions.map(pickSubscription).filter((s): s is AdminSubscription => s !== null)
    : [];
  return {
    user,
    entitlement: {
      ...pickEntitlement(e, user),
      comp_servers: Number.isInteger(e.comp_servers) ? (e.comp_servers as number) : user.comp_servers,
      tier_servers: count(e.tier_servers),
      own_machines: count(e.own_machines),
      can_start: e.can_start === true,
    },
    servers,
    billing: {
      livemode: billing.livemode === true,
      stripe_customer_id: typeof billing.stripe_customer_id === 'string' && billing.stripe_customer_id !== '' ? billing.stripe_customer_id : null,
      subscriptions,
    },
  };
}

function pickUserReply(data: unknown): User | null {
  return pickUser((data as { user?: unknown } | null)?.user);
}

// GET /api/admin/users: every player account, with its entitlement, live
// servers and subscription status.
export function listUsers(signal?: AbortSignal): Promise<AdminResult<UserListEntry[]>> {
  return adminCall(
    () => adminClient.get<unknown>('/users', { signal }),
    (data) => {
      const users = (data as { users?: unknown } | null)?.users;
      if (!Array.isArray(users)) return null;
      const entries = users.map(pickListEntry);
      return entries.some((e) => e === null) ? null : (entries as UserListEntry[]);
    },
  );
}

// GET /api/admin/users/<id>: one account with its entitlement, hosted servers
// and billing ids.
export function fetchUser(id: number, signal?: AbortSignal): Promise<AdminResult<UserDetail>> {
  return adminCall(() => adminClient.get<unknown>(`/users/${id}`, { signal }), pickDetail);
}

// POST /api/admin/users.
export function createUserAdmin(payload: CreateUserPayload): Promise<AdminResult<User>> {
  return adminCall(() => adminClient.post<unknown>('/users', payload), pickUserReply);
}

// PUT /api/admin/users/<id>: a partial update (the profile, the password, the
// role).
export function updateUser(id: number, payload: UpdateUserPayload): Promise<AdminResult<User>> {
  return adminCall(() => adminClient.put<unknown>(`/users/${id}`, payload), pickUserReply);
}

// PUT /api/admin/users/<id>/comp-servers: the admin's grant of hosted servers
// (0 to 16). Lowering it below what the account runs makes the service stop
// its newest servers first; 404 not_found for an unknown account.
export function setCompServers(id: number, compServers: number): Promise<AdminResult<User>> {
  return adminCall(() => adminClient.put<unknown>(`/users/${id}/comp-servers`, { comp_servers: compServers }), pickUserReply);
}

// DELETE /api/admin/users/<id>; 409 active_subscription while a support tier
// still grants.
export async function deleteUser(id: number): Promise<AdminResult<void>> {
  return adminCall(
    () => adminClient.delete<unknown>(`/users/${id}`),
    () => true,
  ).then((r) => (r.ok ? { ok: true as const, value: undefined } : r));
}

export async function registerUser(payload: RegisterPayload): Promise<User> {
  const res = await apiClient.post<UserResponse>('/register', payload);
  return res.data.user;
}
