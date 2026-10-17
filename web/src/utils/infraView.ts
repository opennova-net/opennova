// The infrastructure page's words (pages/admin/InfraSection.vue): each
// program's state, the health of tasks and targets, task sizes and images, and
// what an operator should look at first.

import type { InfraCloudRead, InfraOverview, InfraService, InfraTargetGroup, InfraTask } from '../types/infra';

export type Tone = 'ok' | 'warn' | 'bad' | 'idle';

export function toneBadgeClass(tone: Tone): string {
  const base = 'inline-flex items-center rounded-control px-2 py-0.5 text-xs font-semibold uppercase tracking-wider';
  switch (tone) {
    case 'ok':
      return `${base} bg-online/15 text-online`;
    case 'warn':
      return `${base} bg-warn/15 text-warn`;
    case 'bad':
      return `${base} bg-danger/15 text-danger`;
    default:
      return `${base} bg-hover text-ink-muted`;
  }
}

export function toneTextClass(tone: Tone): string {
  return tone === 'ok' ? 'text-online' : tone === 'warn' ? 'text-warn' : tone === 'bad' ? 'text-danger' : 'text-ink-muted';
}

// The deployment ECS rolls the service to.
export function primaryDeployment(s: InfraService) {
  return s.deployments.find((d) => d.status === 'PRIMARY') ?? null;
}

// A program's state at a glance: stopped (no task wanted), its last rollout
// failed (ECS rolled it back), rolling out (a second deployment, or a task
// starting), short of tasks, or steady.
export function serviceState(s: InfraService): { label: string; tone: Tone } {
  if (s.desired === 0) return { label: 'Stopped', tone: 'idle' };
  const primary = primaryDeployment(s);
  if (primary?.rollout_state === 'FAILED') return { label: 'Rollout failed', tone: 'bad' };
  if (s.deployments.length > 1 || s.pending > 0 || primary?.rollout_state === 'IN_PROGRESS') {
    return { label: 'Rolling out', tone: 'warn' };
  }
  if (s.running < s.desired) return { label: 'Short of tasks', tone: 'bad' };
  return { label: 'Steady', tone: 'ok' };
}

// "In progress", "Completed", "Failed".
export function rolloutLabel(state: string | null): string {
  if (!state) return 'Unknown';
  const words = state.toLowerCase().replace(/_/g, ' ');
  return words.charAt(0).toUpperCase() + words.slice(1);
}

// A task's or a container's health: ECS's container health check.
export function healthLabel(health: string): { label: string; tone: Tone } {
  if (health === 'HEALTHY') return { label: 'Healthy', tone: 'ok' };
  if (health === 'UNHEALTHY') return { label: 'Unhealthy', tone: 'bad' };
  return { label: 'Health unknown', tone: 'idle' };
}

// "Running", "Pending", "Provisioning", "Stopped": ECS's last status.
export function taskStatusLabel(status: string): string {
  if (!status) return 'Unknown';
  return status.charAt(0) + status.slice(1).toLowerCase().replace(/_/g, ' ');
}

// A target's health as the load balancer sees it.
export function targetLabel(state: string): { label: string; tone: Tone } {
  switch (state) {
    case 'healthy':
      return { label: 'Healthy', tone: 'ok' };
    case 'unhealthy':
      return { label: 'Unhealthy', tone: 'bad' };
    case 'initial':
      return { label: 'Registering', tone: 'warn' };
    case 'draining':
      return { label: 'Draining', tone: 'warn' };
    case 'unused':
      return { label: 'Unused', tone: 'idle' };
    case 'unavailable':
      return { label: 'Unavailable', tone: 'bad' };
    default:
      return { label: state || 'Unknown', tone: 'idle' };
  }
}

// What each listener carries.
const LISTENERS: Record<string, string> = {
  gate: 'The gate (UDP 7597)',
  lobby: 'The lobby (UDP 64206)',
  http: 'The retail HTTP port (TCP 8080)',
  https: 'The website (TCP 443)',
  update: 'The updater (TCP 20044, 20556, 21068, 21580)',
};

export function listenerLabel(key: string): string {
  return LISTENERS[key] ?? key;
}

// "0.5 vCPU, 1 GiB": a task's size from its CPU units and MiB.
export function taskSize(task: Pick<InfraTask, 'cpu' | 'memory'>): string | null {
  const cpu = Number(task.cpu);
  const memory = Number(task.memory);
  if (!task.cpu || !task.memory || !Number.isFinite(cpu) || !Number.isFinite(memory)) return null;
  const gib = memory / 1024;
  return `${cpu / 1024} vCPU, ${Number.isInteger(gib) ? gib : gib.toFixed(1)} GiB`;
}

// "opennova-novaworld-web@sha256:0123456789ab": an image's name without its
// registry and with its digest cut to twelve digits.
export function shortImage(image: string): string {
  const name = image.slice(image.lastIndexOf('/') + 1);
  const at = name.indexOf('@sha256:');
  if (at >= 0) return name.slice(0, at + 8 + 12);
  return name;
}

// "sha256:0123456789ab".
export function shortDigest(digest: string): string {
  return digest.startsWith('sha256:') ? digest.slice(0, 7 + 12) : digest;
}

// "web: 1 of 2 running": counts in words.
export function countLabel(n: number, one: string, many = `${one}s`): string {
  return `${n} ${n === 1 ? one : many}`;
}

// The running tasks and the ones wanted, over every program.
export function taskTotals(cloud: InfraCloudRead): { running: number; desired: number; pending: number; unhealthy: number } {
  let running = 0;
  let desired = 0;
  let pending = 0;
  let unhealthy = 0;
  for (const s of cloud.services) {
    running += s.running;
    desired += s.desired;
    pending += s.pending;
    unhealthy += s.tasks.filter((t) => t.health === 'UNHEALTHY').length;
  }
  return { running, desired, pending, unhealthy };
}

export function targetTotals(groups: InfraTargetGroup[]): { healthy: number; total: number } {
  let healthy = 0;
  let total = 0;
  for (const g of groups) {
    total += g.targets.length;
    healthy += g.targets.filter((t) => t.state === 'healthy').length;
  }
  return { healthy, total };
}

// The programs running a revision other than the release's while not rolling
// out: a deploy or a rollback that did not finish.
export function offRelease(cloud: InfraCloudRead): InfraService[] {
  return cloud.services.filter((s) => s.on_release === false && serviceState(s).label !== 'Rolling out');
}

// A program ECS is rolling to a new deployment (a deploy's or a rollback's),
// not one replacing a task.
function isRolling(s: InfraService): boolean {
  return s.deployments.length > 1 || primaryDeployment(s)?.rollout_state === 'IN_PROGRESS';
}

// "web, controller and worker".
function names(services: InfraService[]): string {
  const n = services.map((s) => s.name);
  return n.length < 2 ? n.join('') : `${n.slice(0, -1).join(', ')} and ${n[n.length - 1]}`;
}

// What an operator should look at first, most pressing first.
export function infraWarnings(o: InfraOverview): string[] {
  const out: string[] = [];
  const cloud = o.cloud;
  if (cloud.configured && !cloud.error) {
    for (const s of cloud.services) {
      const state = serviceState(s);
      if (state.label === 'Rollout failed') {
        const reason = primaryDeployment(s)?.rollout_reason;
        out.push(`${s.name}'s last rollout failed${reason ? `: ${reason}` : ''}. ECS rolls a failed deployment back.`);
      } else if (state.label === 'Short of tasks') {
        out.push(`${s.name} has ${s.running} of ${s.desired} tasks running.`);
      }
    }
    for (const m of cloud.missing_services) out.push(`ECS has no service ${m}.`);
    for (const g of cloud.target_groups) {
      const bad = g.targets.filter((t) => t.state === 'unhealthy' || t.state === 'unavailable');
      if (bad.length > 0) {
        const reasons = [...new Set(bad.map((t) => t.reason).filter((r): r is string => !!r))];
        out.push(
          `${listenerLabel(g.key)}: ${countLabel(bad.length, 'target')} unhealthy${reasons.length ? ` (${reasons.join(', ')})` : ''}.`,
        );
      } else if (!g.error && g.targets.length === 0) {
        out.push(`${listenerLabel(g.key)} has no target: nothing behind it answers.`);
      }
    }
    if (cloud.database && cloud.database.status !== 'available') out.push(`The database is ${cloud.database.status}.`);
    if (cloud.cache && cloud.cache.status !== 'available') out.push(`The cache is ${cloud.cache.status}.`);
    // A deploy (or a rollback) rolls the programs in turn and records its
    // release once the last one serves: while one still rolls, the ones it
    // rolled already run revisions the release does not name yet.
    const off = offRelease(cloud);
    const rolling = cloud.services.filter(isRolling);
    if (off.length > 0 && rolling.length > 0) {
      out.push(
        `A deploy is rolling out (${names(rolling)} now): ${names(off)} ${off.length === 1 ? 'runs its new revision' : 'run their new revisions'}, ` +
          'and the release is recorded once every program does.',
      );
    } else {
      for (const s of off) {
        out.push(`${s.name} runs ${s.task_definition}, not the release's revision: a deploy or a rollback did not finish.`);
      }
    }
  }
  if (!o.lobbies.some((l) => l.live && l.state === 'active')) {
    out.push('No lobby instance has been seen taking clients in the last 15 seconds.');
  }
  if (!o.controller.leader) out.push('No controller has been elected yet: nothing schedules the periodic jobs or runs the fleet.');
  return out;
}
