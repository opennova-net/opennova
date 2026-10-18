// The admin's infrastructure view as GET /api/admin/infrastructure answers it
// (apps/novaworld_web/infra/infra_routes.cpp), for its page's and its client's
// tests: a deployment mid-deploy, as tests/novaworld/infra_aws_stub.h has AWS
// answer it (web rolling out revision 43 beside 42, an earlier task of it
// stopped, the nightly backup running, 443 with an unhealthy target), and the
// local stack's view, which reads no AWS. NOW is the reply's `at`.

import type {
  InfraCloudRead,
  InfraLobby,
  InfraMachine,
  InfraOverview,
  InfraService,
  InfraTask,
} from '../types/infra';

export const NOW = Date.UTC(2026, 9, 17, 12, 1, 30);

// An ISO time `seconds` before NOW.
export function ago(seconds: number): string {
  return new Date(NOW - seconds * 1000).toISOString().replace(/\.\d{3}Z$/, 'Z');
}

const DIGEST = 'sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef';

export function task(id: string, program: string | null, overrides: Partial<InfraTask> = {}): InfraTask {
  const name = program ?? 'backup';
  return {
    id,
    program,
    group: program ? `service:${program}` : `family:opennova-prod-${name}`,
    task_definition: `opennova-prod-${name}:7`,
    on_release: program ? true : null,
    last_status: 'RUNNING',
    desired_status: 'RUNNING',
    health: 'HEALTHY',
    created_at: ago(5000),
    started_at: ago(4990),
    stopping_at: null,
    stopped_at: null,
    stopped_reason: null,
    stop_code: null,
    zone: 'us-east-1a',
    private_ip: '10.40.0.11',
    cpu: '256',
    memory: '512',
    containers: [
      {
        name,
        image: `ghcr.io/opennova-net/opennova-novaworld-${name}@${DIGEST}`,
        image_digest: DIGEST,
        last_status: 'RUNNING',
        health: 'HEALTHY',
        exit_code: null,
        reason: null,
      },
    ],
    ...overrides,
  };
}

export function service(name: string, overrides: Partial<InfraService> = {}): InfraService {
  return {
    name,
    status: 'ACTIVE',
    desired: 1,
    running: 1,
    pending: 0,
    task_definition: `opennova-prod-${name}:7`,
    on_release: true,
    deployments: [
      {
        id: `ecs-svc/${name}`,
        status: 'PRIMARY',
        rollout_state: 'COMPLETED',
        rollout_reason: null,
        task_definition: `opennova-prod-${name}:7`,
        desired: 1,
        running: 1,
        pending: 0,
        failed: 0,
        created_at: ago(5000),
        updated_at: ago(4700),
      },
    ],
    events: [],
    tasks: [task(`${name}0000aaaa1111aaaa1111aaaa11`, name)],
    stopped_tasks: [],
    ...overrides,
  };
}

// Web mid-rollout: revision 43 starting beside 42, the release's; an earlier
// task of 43 stopped, its container exited 1.
export function rollingWeb(): InfraService {
  return service('web', {
    desired: 2,
    running: 1,
    pending: 1,
    task_definition: 'opennova-prod-web:43',
    on_release: false,
    deployments: [
      {
        id: 'ecs-svc/4',
        status: 'PRIMARY',
        rollout_state: 'IN_PROGRESS',
        rollout_reason: 'ECS deployment ecs-svc/4 in progress.',
        task_definition: 'opennova-prod-web:43',
        desired: 1,
        running: 0,
        pending: 1,
        failed: 1,
        created_at: ago(90),
        updated_at: ago(30),
      },
      {
        id: 'ecs-svc/3',
        status: 'ACTIVE',
        rollout_state: 'COMPLETED',
        rollout_reason: null,
        task_definition: 'opennova-prod-web:42',
        desired: 1,
        running: 1,
        pending: 0,
        failed: 0,
        created_at: ago(5000),
        updated_at: ago(4700),
      },
    ],
    events: [
      { at: ago(30), message: '(service web) has started 1 tasks: (task 3333cccc).' },
      { at: ago(60), message: '(service web) has stopped 1 running tasks: (task 9999ffff).' },
    ],
    tasks: [
      task('4444dddd4444dddd4444dddd4444dddd', 'web', {
        task_definition: 'opennova-prod-web:42',
        private_ip: '10.40.0.14',
        cpu: '512',
        memory: '1024',
      }),
      task('3333cccc3333cccc3333cccc3333cccc', 'web', {
        task_definition: 'opennova-prod-web:43',
        on_release: false,
        last_status: 'PENDING',
        health: 'UNKNOWN',
        created_at: ago(35),
        started_at: null,
        zone: 'us-east-1b',
        private_ip: '10.40.1.13',
        cpu: '512',
        memory: '1024',
      }),
    ],
    stopped_tasks: [
      task('9999ffff9999ffff9999ffff9999ffff', 'web', {
        task_definition: 'opennova-prod-web:43',
        on_release: false,
        last_status: 'STOPPED',
        desired_status: 'STOPPED',
        health: 'UNHEALTHY',
        stopped_at: ago(60),
        stopped_reason: 'Essential container in task exited',
        stop_code: 'EssentialContainerExited',
        containers: [
          {
            name: 'web',
            image: `ghcr.io/opennova-net/opennova-novaworld-web@${DIGEST}`,
            image_digest: null,
            last_status: 'STOPPED',
            health: 'UNHEALTHY',
            exit_code: 1,
            reason: null,
          },
        ],
      }),
    ],
  });
}

export function cloud(overrides: Partial<InfraCloudRead> = {}): InfraCloudRead {
  return {
    configured: true,
    read_at: ago(5),
    cluster: 'opennova-prod',
    region: 'us-east-1',
    error: null,
    release: { id: '20261017T120000Z-abc1234', created: '2026-10-17T12:00:00Z', revision: 'abc1234def56' },
    release_none: false,
    release_error: null,
    services: [
      service('gate', {
        events: [{ at: ago(2400), message: '(service gate) has reached a steady state.' }],
      }),
      service('lobby'),
      rollingWeb(),
      service('controller'),
      service('worker'),
    ],
    missing_services: [],
    services_error: null,
    one_off_tasks: [task('7777aaaa7777aaaa7777aaaa7777aaaa', null, { health: 'UNKNOWN', started_at: ago(600) })],
    one_off_stopped_tasks: [],
    tasks_error: null,
    target_groups: [
      { key: 'gate', name: 'opennova-prod-gate', targets: [{ id: '10.40.0.11', port: 7597, zone: 'us-east-1a', state: 'healthy', reason: null, description: null }], error: null },
      { key: 'lobby', name: 'opennova-prod-lobby', targets: [{ id: '10.40.1.12', port: 64206, zone: 'us-east-1b', state: 'healthy', reason: null, description: null }], error: null },
      {
        key: 'http',
        name: 'opennova-prod-http',
        targets: [
          { id: '10.40.0.14', port: 8080, zone: 'us-east-1a', state: 'healthy', reason: null, description: null },
          {
            id: '10.40.1.13',
            port: 8080,
            zone: 'us-east-1b',
            state: 'initial',
            reason: 'Elb.RegistrationInProgress',
            description: 'Target registration is in progress',
          },
        ],
        error: null,
      },
      {
        key: 'https',
        name: 'opennova-prod-https',
        targets: [
          { id: '10.40.0.14', port: 443, zone: 'us-east-1a', state: 'healthy', reason: null, description: null },
          {
            id: '10.40.0.19',
            port: 443,
            zone: 'us-east-1a',
            state: 'unhealthy',
            reason: 'Target.FailedHealthChecks',
            description: 'Health checks failed',
          },
        ],
        error: null,
      },
    ],
    database: {
      identifier: 'opennova-prod-db',
      status: 'available',
      engine: 'postgres',
      engine_version: '17.4',
      instance_class: 'db.t4g.small',
      multi_az: true,
      zone: 'us-east-1a',
      secondary_zone: 'us-east-1b',
      storage_type: 'gp3',
      storage_gb: 20,
    },
    database_error: null,
    cache: {
      id: 'opennova-prod-cache',
      status: 'available',
      multi_az: true,
      automatic_failover: 'enabled',
      node_type: 'cache.t4g.micro',
      engine: 'valkey',
      nodes: [
        { id: 'opennova-prod-cache-001', role: 'primary', zone: 'us-east-1a' },
        { id: 'opennova-prod-cache-002', role: 'replica', zone: 'us-east-1b' },
      ],
    },
    cache_error: null,
    machines_error: null,
    ...overrides,
  };
}

export function lobby(id: number, overrides: Partial<InfraLobby> = {}): InfraLobby {
  return {
    id,
    endpoint: '54.162.79.147:64206',
    state: 'active',
    started_at: ago(5000),
    last_seen_at: ago(2),
    seen_ago_seconds: 2,
    live: true,
    sessions: 3,
    listings: 1,
    ...overrides,
  };
}

export function machine(id: number, overrides: Partial<InfraMachine> = {}): InfraMachine {
  return {
    id,
    provider: 'ec2',
    kind: 'shared',
    state: 'ready',
    slots: 12,
    slots_used: 4,
    cloud_id: 'i-0aaaaaaaaaaaaaaa1',
    instance_type: 't3a.large',
    cloud_state: 'running',
    launched_at: ago(7200),
    ready_at: ago(6900),
    ended_at: null,
    end_reason: null,
    zone: 'us-east-1b',
    ec2_state: 'running',
    ...overrides,
  };
}

export function overview(overrides: Partial<InfraOverview> = {}): InfraOverview {
  return {
    at: ago(0),
    cloud: cloud(),
    lobbies: [lobby(1), lobby(2, { state: 'draining', live: false, last_seen_at: ago(120), seen_ago_seconds: 120, sessions: 0, listings: 0 })],
    fleet: { leader: 'fleet-1234abcd', generation: 3, elected_at: ago(3600) },
    hosting: { enabled: true, provider: 'ec2' },
    machines: [
      machine(1),
      machine(2, {
        state: 'ended',
        slots_used: 0,
        cloud_id: 'i-0bbbbbbbbbbbbbbb2',
        cloud_state: null,
        ended_at: ago(1800),
        end_reason: 'drained',
        zone: null,
        ec2_state: null,
      }),
    ],
    ...overrides,
  };
}

// The local stack's view: no AWS, the database's sections alone.
export function localOverview(): InfraOverview {
  return overview({
    cloud: {
      configured: false,
      unavailable:
        'AWS is not read here: ONNET_INFRA_CLUSTER is unset (the local stack runs no ECS services, load balancer, RDS or ElastiCache)',
    },
    hosting: { enabled: true, provider: 'docker' },
    machines: [machine(1, { provider: 'docker', cloud_id: 'dev-machine-1', instance_type: null, zone: null, ec2_state: null })],
  });
}
