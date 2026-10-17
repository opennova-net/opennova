// The admin's infrastructure view (GET /api/admin/infrastructure,
// apps/novaworld_web/infra/infra_routes.h): the deployment as it runs, the
// cloud's sections from AWS and the rest from the database. Answers an
// AdminResult (admin.ts); a reply that does not read as the view is malformed.

import { adminClient } from './client';
import { adminCall, type AdminResult } from './admin';
import type {
  InfraCache,
  InfraCloud,
  InfraContainer,
  InfraController,
  InfraDatabase,
  InfraDeployment,
  InfraEvent,
  InfraLobby,
  InfraMachine,
  InfraOverview,
  InfraRelease,
  InfraService,
  InfraTarget,
  InfraTargetGroup,
  InfraTask,
} from '../types/infra';

type Fields = Record<string, unknown>;

const isObject = (v: unknown): v is Fields => typeof v === 'object' && v !== null && !Array.isArray(v);
const isCount = (v: unknown): v is number => Number.isInteger(v) && (v as number) >= 0;
const count = (v: unknown): number => (isCount(v) ? v : 0);
const text = (v: unknown): string => (typeof v === 'string' ? v : '');
const textOrNull = (v: unknown): string | null => (typeof v === 'string' && v !== '' ? v : null);
const boolOrNull = (v: unknown): boolean | null => (typeof v === 'boolean' ? v : null);

// Each element `pick` reads; null when the value is no array or an element does not read.
function list<T>(value: unknown, pick: (v: unknown) => T | null): T[] | null {
  if (!Array.isArray(value)) return null;
  const out: T[] = [];
  for (const v of value) {
    const picked = pick(v);
    if (picked === null) return null;
    out.push(picked);
  }
  return out;
}

function pickContainer(v: unknown): InfraContainer | null {
  if (!isObject(v) || typeof v.name !== 'string') return null;
  return {
    name: v.name,
    image: text(v.image),
    image_digest: textOrNull(v.image_digest),
    last_status: text(v.last_status),
    health: text(v.health) || 'UNKNOWN',
    exit_code: Number.isInteger(v.exit_code) ? (v.exit_code as number) : null,
    reason: textOrNull(v.reason),
  };
}

export function pickTask(v: unknown): InfraTask | null {
  if (!isObject(v) || typeof v.id !== 'string' || v.id === '') return null;
  const containers = list(v.containers ?? [], pickContainer);
  if (containers === null) return null;
  return {
    id: v.id,
    program: textOrNull(v.program),
    group: text(v.group),
    task_definition: text(v.task_definition),
    on_release: boolOrNull(v.on_release),
    last_status: text(v.last_status),
    desired_status: text(v.desired_status),
    health: text(v.health) || 'UNKNOWN',
    created_at: textOrNull(v.created_at),
    started_at: textOrNull(v.started_at),
    stopping_at: textOrNull(v.stopping_at),
    stopped_at: textOrNull(v.stopped_at),
    stopped_reason: textOrNull(v.stopped_reason),
    stop_code: textOrNull(v.stop_code),
    zone: textOrNull(v.zone),
    private_ip: textOrNull(v.private_ip),
    cpu: textOrNull(v.cpu),
    memory: textOrNull(v.memory),
    containers,
  };
}

function pickDeployment(v: unknown): InfraDeployment | null {
  if (!isObject(v) || typeof v.status !== 'string') return null;
  return {
    id: text(v.id),
    status: v.status,
    rollout_state: textOrNull(v.rollout_state),
    rollout_reason: textOrNull(v.rollout_reason),
    task_definition: text(v.task_definition),
    desired: count(v.desired),
    running: count(v.running),
    pending: count(v.pending),
    failed: count(v.failed),
    created_at: textOrNull(v.created_at),
    updated_at: textOrNull(v.updated_at),
  };
}

function pickEvent(v: unknown): InfraEvent | null {
  if (!isObject(v) || typeof v.message !== 'string') return null;
  return { at: textOrNull(v.at), message: v.message };
}

function pickService(v: unknown): InfraService | null {
  if (!isObject(v) || typeof v.name !== 'string' || !isCount(v.desired) || !isCount(v.running)) return null;
  const deployments = list(v.deployments ?? [], pickDeployment);
  const events = list(v.events ?? [], pickEvent);
  const tasks = list(v.tasks ?? [], pickTask);
  const stopped = list(v.stopped_tasks ?? [], pickTask);
  if (!deployments || !events || !tasks || !stopped) return null;
  return {
    name: v.name,
    status: text(v.status),
    desired: v.desired,
    running: v.running,
    pending: count(v.pending),
    task_definition: text(v.task_definition),
    on_release: boolOrNull(v.on_release),
    deployments,
    events,
    tasks,
    stopped_tasks: stopped,
  };
}

function pickTarget(v: unknown): InfraTarget | null {
  if (!isObject(v) || typeof v.id !== 'string' || typeof v.state !== 'string') return null;
  return {
    id: v.id,
    port: count(v.port),
    zone: textOrNull(v.zone),
    state: v.state,
    reason: textOrNull(v.reason),
    description: textOrNull(v.description),
  };
}

function pickTargetGroup(v: unknown): InfraTargetGroup | null {
  if (!isObject(v) || typeof v.key !== 'string') return null;
  const targets = list(v.targets ?? [], pickTarget);
  if (!targets) return null;
  return { key: v.key, name: text(v.name), targets, error: textOrNull(v.error) };
}

function pickDatabase(v: unknown): InfraDatabase | null {
  if (!isObject(v) || typeof v.status !== 'string') return null;
  return {
    identifier: text(v.identifier),
    status: v.status,
    engine: text(v.engine),
    engine_version: text(v.engine_version),
    instance_class: text(v.instance_class),
    multi_az: v.multi_az === true,
    zone: textOrNull(v.zone),
    secondary_zone: textOrNull(v.secondary_zone),
    storage_type: textOrNull(v.storage_type),
    storage_gb: count(v.storage_gb),
  };
}

function pickCache(v: unknown): InfraCache | null {
  if (!isObject(v) || typeof v.status !== 'string') return null;
  const nodes = list(v.nodes ?? [], (n) =>
    isObject(n) && typeof n.id === 'string' ? { id: n.id, role: textOrNull(n.role), zone: textOrNull(n.zone) } : null,
  );
  if (!nodes) return null;
  return {
    id: text(v.id),
    status: v.status,
    multi_az: v.multi_az === true,
    automatic_failover: textOrNull(v.automatic_failover),
    node_type: textOrNull(v.node_type),
    engine: textOrNull(v.engine),
    nodes,
  };
}

function pickRelease(v: unknown): InfraRelease | null {
  if (!isObject(v) || typeof v.id !== 'string') return null;
  return { id: v.id, created: textOrNull(v.created), revision: textOrNull(v.revision) };
}

// A section that may be null: null stays null, anything else must read.
function optional<T>(value: unknown, pick: (v: unknown) => T | null): { ok: boolean; value: T | null } {
  if (value === null || value === undefined) return { ok: true, value: null };
  const picked = pick(value);
  return { ok: picked !== null, value: picked };
}

export function pickCloud(v: unknown): InfraCloud | null {
  if (!isObject(v) || typeof v.configured !== 'boolean') return null;
  if (!v.configured) return { configured: false, unavailable: text(v.unavailable) || 'AWS is not read here.' };
  const services = list(v.services ?? [], pickService);
  const missing = list(v.missing_services ?? [], (m) => (typeof m === 'string' ? m : null));
  const oneOff = list(v.one_off_tasks ?? [], pickTask);
  const oneOffStopped = list(v.one_off_stopped_tasks ?? [], pickTask);
  const groups = list(v.target_groups ?? [], pickTargetGroup);
  const release = optional(v.release, pickRelease);
  const database = optional(v.database, pickDatabase);
  const cache = optional(v.cache, pickCache);
  if (!services || !missing || !oneOff || !oneOffStopped || !groups || !release.ok || !database.ok || !cache.ok) return null;
  return {
    configured: true,
    read_at: text(v.read_at),
    cluster: text(v.cluster),
    region: text(v.region),
    error: textOrNull(v.error),
    release: release.value,
    release_none: v.release_none === true,
    release_error: textOrNull(v.release_error),
    services,
    missing_services: missing,
    services_error: textOrNull(v.services_error),
    one_off_tasks: oneOff,
    one_off_stopped_tasks: oneOffStopped,
    tasks_error: textOrNull(v.tasks_error),
    target_groups: groups,
    database: database.value,
    database_error: textOrNull(v.database_error),
    cache: cache.value,
    cache_error: textOrNull(v.cache_error),
    machines_error: textOrNull(v.machines_error),
  };
}

function pickLobby(v: unknown): InfraLobby | null {
  if (!isObject(v) || !isCount(v.id) || typeof v.endpoint !== 'string') return null;
  return {
    id: v.id,
    endpoint: v.endpoint,
    state: text(v.state),
    started_at: text(v.started_at),
    last_seen_at: text(v.last_seen_at),
    seen_ago_seconds: count(v.seen_ago_seconds),
    live: v.live === true,
    sessions: count(v.sessions),
    listings: count(v.listings),
  };
}

function pickMachine(v: unknown): InfraMachine | null {
  if (!isObject(v) || !isCount(v.id) || typeof v.state !== 'string') return null;
  return {
    id: v.id,
    provider: text(v.provider),
    kind: text(v.kind),
    state: v.state,
    slots: count(v.slots),
    slots_used: count(v.slots_used),
    cloud_id: textOrNull(v.cloud_id),
    instance_type: textOrNull(v.instance_type),
    cloud_state: textOrNull(v.cloud_state),
    launched_at: textOrNull(v.launched_at),
    ready_at: textOrNull(v.ready_at),
    ended_at: textOrNull(v.ended_at),
    end_reason: textOrNull(v.end_reason),
    zone: textOrNull(v.zone),
    ec2_state: textOrNull(v.ec2_state),
  };
}

function pickController(v: unknown): InfraController | null {
  if (!isObject(v)) return null;
  return { leader: textOrNull(v.leader), generation: count(v.generation), elected_at: textOrNull(v.elected_at) };
}

export function pickOverview(v: unknown): InfraOverview | null {
  if (!isObject(v) || typeof v.at !== 'string') return null;
  const cloud = pickCloud(v.cloud);
  const lobbies = list(v.lobbies, pickLobby);
  const controller = pickController(v.controller);
  const machines = list(v.machines, pickMachine);
  if (!cloud || !lobbies || !controller || !machines || !isObject(v.hosting)) return null;
  return {
    at: v.at,
    cloud,
    lobbies,
    controller,
    hosting: { enabled: v.hosting.enabled === true, provider: text(v.hosting.provider) || 'none' },
    machines,
  };
}

// GET /api/admin/infrastructure.
export function fetchInfrastructure(signal?: AbortSignal): Promise<AdminResult<InfraOverview>> {
  return adminCall(() => adminClient.get<unknown>('/infrastructure', { signal }), pickOverview);
}
