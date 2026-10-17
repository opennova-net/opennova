// The admin's infrastructure view as GET /api/admin/infrastructure answers it
// (apps/novaworld_web/infra/infra_routes.h). Times are ISO 8601 UTC. The cloud's
// sections come from AWS, read by web at most every 20 seconds; the lobbies,
// the controller and the machines from the database.

export interface InfraContainer {
  name: string;
  // As the task definition names it: a release pins it by digest.
  image: string;
  // The digest ECS pulled; null until it has.
  image_digest: string | null;
  last_status: string;
  // HEALTHY, UNHEALTHY or UNKNOWN.
  health: string;
  exit_code: number | null;
  reason: string | null;
}

export interface InfraTask {
  // The task ARN's last segment.
  id: string;
  // The service it runs for; null for a one-off task (the backup, a db task).
  program: string | null;
  // "service:<name>" or "family:<task definition family>".
  group: string;
  // "<family>:<revision>".
  task_definition: string;
  // Whether that is the revision the release names for its program; null
  // when the release names none.
  on_release: boolean | null;
  last_status: string;
  desired_status: string;
  health: string;
  created_at: string | null;
  started_at: string | null;
  stopping_at: string | null;
  stopped_at: string | null;
  stopped_reason: string | null;
  stop_code: string | null;
  zone: string | null;
  private_ip: string | null;
  // CPU units ("512": half a vCPU) and MiB.
  cpu: string | null;
  memory: string | null;
  containers: InfraContainer[];
}

export interface InfraDeployment {
  id: string;
  // PRIMARY (the one ECS rolls to), ACTIVE (an older one still running) or INACTIVE.
  status: string;
  // COMPLETED, IN_PROGRESS or FAILED.
  rollout_state: string | null;
  rollout_reason: string | null;
  task_definition: string;
  desired: number;
  running: number;
  pending: number;
  failed: number;
  created_at: string | null;
  updated_at: string | null;
}

export interface InfraEvent {
  at: string | null;
  message: string;
}

export interface InfraService {
  name: string;
  status: string;
  desired: number;
  running: number;
  pending: number;
  task_definition: string;
  on_release: boolean | null;
  deployments: InfraDeployment[];
  // The newest ten, newest first.
  events: InfraEvent[];
  tasks: InfraTask[];
  // The newest three ECS still lists.
  stopped_tasks: InfraTask[];
}

export interface InfraTarget {
  // The task's address.
  id: string;
  port: number;
  zone: string | null;
  // initial, healthy, unhealthy, draining, unused, unavailable.
  state: string;
  reason: string | null;
  description: string | null;
}

export interface InfraTargetGroup {
  // gate, lobby, http (the retail port) or https (443).
  key: string;
  name: string;
  targets: InfraTarget[];
  error: string | null;
}

export interface InfraDatabase {
  identifier: string;
  status: string;
  engine: string;
  engine_version: string;
  instance_class: string;
  multi_az: boolean;
  zone: string | null;
  secondary_zone: string | null;
  storage_type: string | null;
  storage_gb: number;
}

export interface InfraCacheNode {
  id: string;
  role: string | null;
  zone: string | null;
}

export interface InfraCache {
  id: string;
  status: string;
  multi_az: boolean;
  automatic_failover: string | null;
  node_type: string | null;
  engine: string | null;
  nodes: InfraCacheNode[];
}

export interface InfraRelease {
  id: string;
  created: string | null;
  // The commit it was cut from.
  revision: string | null;
}

// AWS read: each section, or the error that kept it out.
export interface InfraCloudRead {
  configured: true;
  read_at: string;
  cluster: string;
  region: string;
  // The read failed as a whole (the task role's credentials): nothing else holds.
  error: string | null;
  release: InfraRelease | null;
  // No release recorded yet (before the first app deploy).
  release_none: boolean;
  release_error: string | null;
  services: InfraService[];
  missing_services: string[];
  services_error: string | null;
  one_off_tasks: InfraTask[];
  one_off_stopped_tasks: InfraTask[];
  tasks_error: string | null;
  target_groups: InfraTargetGroup[];
  database: InfraDatabase | null;
  database_error: string | null;
  cache: InfraCache | null;
  cache_error: string | null;
  machines_error: string | null;
}

// AWS not read here (the local stack), and why.
export interface InfraCloudUnavailable {
  configured: false;
  unavailable: string;
}

export type InfraCloud = InfraCloudRead | InfraCloudUnavailable;

export interface InfraLobby {
  id: number;
  endpoint: string;
  // active (takes clients) or draining.
  state: string;
  started_at: string;
  last_seen_at: string;
  seen_ago_seconds: number;
  // Seen in the last 15 seconds.
  live: boolean;
  sessions: number;
  listings: number;
}

export interface InfraController {
  leader: string | null;
  generation: number;
  elected_at: string | null;
}

export interface InfraMachine {
  id: number;
  provider: string;
  kind: string;
  state: string;
  slots: number;
  slots_used: number;
  cloud_id: string | null;
  instance_type: string | null;
  // What the worker's last describe said of it.
  cloud_state: string | null;
  launched_at: string | null;
  ready_at: string | null;
  ended_at: string | null;
  end_reason: string | null;
  // EC2's own view, when AWS was read and lists it.
  zone: string | null;
  ec2_state: string | null;
}

export interface InfraOverview {
  // The database's now.
  at: string;
  cloud: InfraCloud;
  lobbies: InfraLobby[];
  controller: InfraController;
  hosting: { enabled: boolean; provider: string };
  machines: InfraMachine[];
}
