<template>
  <div ref="root" class="space-y-10">
    <AdminPageHeader
      title="Infrastructure"
      description="The deployment as it runs, read from AWS and the database."
      :loaded-at="loadedAt"
      :now="now"
      :loading="poll.loading.value"
      @refresh="poll.refresh"
    />

    <!-- A live region, always in the page, the box itself while it holds something. -->
    <div role="alert" :class="error ? 'rounded-panel border border-danger/40 bg-danger/10 p-4 text-sm text-danger' : 'sr-only'">{{ error }}</div>
    <p v-if="pollTrouble" class="text-xs text-warn">The last refresh did not get through; this page keeps trying.</p>
    <div v-if="overview === null && poll.loading.value" class="flex h-32 items-center justify-center text-ink-muted">
      Loading the deployment…
    </div>

    <template v-if="overview">
      <p v-if="!overview.cloud.configured" class="infra-unavailable rounded-panel border border-border bg-panel p-4 text-sm text-ink-muted">
        {{ overview.cloud.unavailable }}. What the programs use, the lobby instances, the fleet leader and the machines
        below come from the database.
      </p>
      <p v-else-if="cloud?.error" class="infra-cloud-error rounded-panel border border-danger/40 bg-danger/10 p-4 text-sm text-danger">
        AWS could not be read: {{ cloud.error }}
      </p>

      <ul v-if="warnings.length" class="infra-warnings space-y-2">
        <li v-for="w in warnings" :key="w" class="rounded-panel border border-warn/40 bg-warn/10 p-4 text-sm text-warn">{{ w }}</li>
      </ul>

      <dl class="infra-tiles grid grid-cols-1 gap-3 min-[420px]:grid-cols-2 lg:grid-cols-3">
        <StatTile label="Programs" :value="programsTile.value" :detail="programsTile.detail" />
        <StatTile label="Tasks running" :value="tasksTile.value" :detail="tasksTile.detail" />
        <StatTile label="Load balancer targets" :value="targetsTile.value" :detail="targetsTile.detail" />
        <StatTile label="Release" :value="releaseTile.value" :detail="releaseTile.detail" />
        <StatTile label="Lobby instances" :value="lobbiesTile.value" :detail="lobbiesTile.detail" />
        <StatTile label="Fleet machines" :value="machinesTile.value" :detail="machinesTile.detail" />
      </dl>

      <!-- The programs: a card each (ECS's service, what its tasks use).
           hostd's daemon and the fleet cluster are the Fleet page's. -->
      <section v-if="programNames.length" class="space-y-4" aria-labelledby="infra-programs">
        <div class="flex flex-col gap-1 sm:flex-row sm:items-baseline sm:justify-between">
          <h2 id="infra-programs" class="text-xl font-semibold text-ink">Programs</h2>
          <p v-if="cloudRead" class="text-xs text-ink-muted">
            Cluster <span class="font-mono text-ink">{{ cloudRead.cluster }}</span> in {{ cloudRead.region }}, read from AWS
            <RelativeTime :value="cloudRead.read_at" :now="now" />
          </p>
        </div>
        <template v-if="cloudRead">
          <p v-if="cloudRead.services_error" class="text-sm text-danger">The services could not be read: {{ cloudRead.services_error }}</p>
          <p v-if="cloudRead.tasks_error" class="text-sm text-danger">The tasks could not be read: {{ cloudRead.tasks_error }}</p>
          <p v-if="cloudRead.release_error" class="text-sm text-danger">The release could not be read: {{ cloudRead.release_error }}</p>
        </template>
        <p v-if="programsError" class="infra-programs-error text-sm text-danger">What the programs use could not be read: {{ programsError }}</p>
        <div class="grid gap-4 xl:grid-cols-2">
          <ProgramCard
            v-for="name in programNames"
            :key="name"
            :name="name"
            :service="serviceOf(name)"
            :usage="usageOf(name)"
            :cloud-read="cloudRead !== null"
            :now="now"
            :to="`/admin/infrastructure/programs/${name}`"
          />
        </div>

        <div v-if="cloudRead && (cloudRead.one_off_tasks.length || cloudRead.one_off_stopped_tasks.length)" class="infra-one-off rounded-panel border border-border bg-panel p-4">
          <h3 class="text-sm font-semibold text-ink">One-off tasks</h3>
          <p class="text-xs text-ink-muted">The nightly backup, and the toolbox's database tasks.</p>
          <ul class="mt-2 space-y-1 text-sm">
            <li v-for="t in cloudRead.one_off_tasks" :key="t.id" class="flex flex-wrap gap-x-3">
              <span class="font-mono" :title="`Task ${t.id}`">{{ t.id.slice(0, 8) }}</span>
              <span>{{ t.group.replace(/^family:/, '') }}</span>
              <span class="text-xs text-ink-muted">
                {{ taskStatusLabel(t.last_status) }}<template v-if="t.started_at">, started <RelativeTime :value="t.started_at" :now="now" /></template>
              </span>
            </li>
            <li v-for="t in cloudRead.one_off_stopped_tasks" :key="t.id" class="flex flex-wrap gap-x-3 text-xs text-ink-muted">
              <span class="font-mono text-ink" :title="`Task ${t.id}`">{{ t.id.slice(0, 8) }}</span>
              <span>{{ t.group.replace(/^family:/, '') }}</span>
              <span>
                <template v-if="t.stopped_at">Stopped <RelativeTime :value="t.stopped_at" :now="now" />: </template>{{ stoppedReason(t) }}
              </span>
            </li>
          </ul>
        </div>
      </section>

      <template v-if="cloud && !cloud.error">
        <!-- The load balancer's target groups. -->
        <section class="space-y-4" aria-labelledby="infra-targets">
          <div class="flex flex-col gap-1 sm:flex-row sm:items-baseline sm:justify-between">
            <h2 id="infra-targets" class="text-xl font-semibold text-ink">Load balancer</h2>
            <RouterLink to="/admin/infrastructure/resources/nlb" class="infra-resource-link text-sm text-accent hover:text-ink-bright">
              Its traffic and healthy targets over time
            </RouterLink>
          </div>
          <p class="text-sm text-ink-muted">
            Each listener's targets, the tasks behind it, as the network load balancer's health checks see them.
          </p>
          <div class="relative overflow-x-auto rounded-panel border border-border bg-panel">
            <table class="min-w-full divide-y divide-border">
              <caption class="sr-only">Load balancer targets</caption>
              <thead>
                <tr :class="headRow">
                  <th scope="col" class="px-4 py-3">Listener</th>
                  <th scope="col" class="px-4 py-3">Target</th>
                  <th scope="col" class="px-4 py-3">Zone</th>
                  <th scope="col" class="px-4 py-3">Health</th>
                  <th scope="col" class="px-4 py-3">Why</th>
                </tr>
              </thead>
              <tbody class="divide-y divide-border text-sm text-ink">
                <tr v-if="cloud.target_groups.length === 0">
                  <td colspan="5" class="px-4 py-6 text-center text-ink-muted">No target group is configured here.</td>
                </tr>
                <template v-for="g in cloud.target_groups" :key="g.key">
                  <tr v-if="g.error" class="infra-target-row">
                    <th scope="row" class="whitespace-nowrap px-4 py-3 text-left font-normal">{{ listenerLabel(g.key) }}</th>
                    <td colspan="4" class="px-4 py-3 text-danger">Could not be read: {{ g.error }}</td>
                  </tr>
                  <tr v-else-if="g.targets.length === 0" class="infra-target-row">
                    <th scope="row" class="whitespace-nowrap px-4 py-3 text-left font-normal">{{ listenerLabel(g.key) }}</th>
                    <td colspan="4" class="px-4 py-3 text-warn">No target</td>
                  </tr>
                  <tr v-for="t in g.error ? [] : g.targets" :key="`${g.key} ${t.id}:${t.port}`" class="infra-target-row align-top hover:bg-hover">
                    <th scope="row" class="whitespace-nowrap px-4 py-3 text-left font-normal">
                      {{ listenerLabel(g.key) }}
                      <span class="block font-mono text-xs text-ink-muted">{{ g.name }}</span>
                    </th>
                    <td class="whitespace-nowrap px-4 py-3 font-mono text-xs">{{ t.id }}:{{ t.port }}</td>
                    <td class="whitespace-nowrap px-4 py-3 text-xs">{{ t.zone || 'Unknown' }}</td>
                    <td class="whitespace-nowrap px-4 py-3">
                      <span :class="toneBadgeClass(targetLabel(t.state).tone)">{{ targetLabel(t.state).label }}</span>
                    </td>
                    <td class="px-4 py-3 text-xs text-ink-muted">
                      <template v-if="t.reason || t.description">
                        {{ t.description || t.reason }}
                        <span v-if="t.reason && t.description" class="block font-mono">{{ t.reason }}</span>
                      </template>
                    </td>
                  </tr>
                </template>
              </tbody>
            </table>
          </div>
        </section>

        <!-- The database and the cache. -->
        <section class="space-y-4" aria-labelledby="infra-data">
          <h2 id="infra-data" class="text-xl font-semibold text-ink">Database and cache</h2>
          <div class="grid gap-4 md:grid-cols-2">
            <article class="infra-database space-y-2 rounded-panel border border-border bg-panel p-4" aria-labelledby="infra-database-heading">
              <div class="flex items-start justify-between gap-3">
                <div>
                  <h3 id="infra-database-heading" class="font-semibold text-ink">PostgreSQL (RDS)</h3>
                  <p v-if="cloud.database" class="font-mono text-xs text-ink-muted">{{ cloud.database.identifier }}</p>
                </div>
                <span v-if="cloud.database" :class="toneBadgeClass(dataTone(cloud.database.status))">{{ cloud.database.status }}</span>
              </div>
              <p v-if="cloud.database_error" class="text-sm text-danger">{{ cloud.database_error }}</p>
              <dl v-if="cloud.database" class="grid grid-cols-2 gap-x-4 gap-y-1 text-sm">
                <dt class="text-ink-muted">Engine</dt>
                <dd>{{ cloud.database.engine }} {{ cloud.database.engine_version }}</dd>
                <dt class="text-ink-muted">Class</dt>
                <dd class="font-mono text-xs">{{ cloud.database.instance_class }}</dd>
                <dt class="text-ink-muted">Multi-AZ</dt>
                <dd>{{ cloud.database.multi_az ? 'Yes' : 'No' }}</dd>
                <dt class="text-ink-muted">Zones</dt>
                <dd>
                  {{ cloud.database.zone || 'Unknown' }}<template v-if="cloud.database.secondary_zone">, standby in {{ cloud.database.secondary_zone }}</template>
                </dd>
                <dt class="text-ink-muted">Storage</dt>
                <dd>{{ cloud.database.storage_gb }} GiB{{ cloud.database.storage_type ? ` ${cloud.database.storage_type}` : '' }}</dd>
              </dl>
              <RouterLink to="/admin/infrastructure/resources/rds" class="infra-resource-link block text-sm text-accent hover:text-ink-bright">
                Its CPU, memory, connections and network over time
              </RouterLink>
            </article>
            <article class="infra-cache space-y-2 rounded-panel border border-border bg-panel p-4" aria-labelledby="infra-cache-heading">
              <div class="flex items-start justify-between gap-3">
                <div>
                  <h3 id="infra-cache-heading" class="font-semibold text-ink">Valkey (ElastiCache)</h3>
                  <p v-if="cloud.cache" class="font-mono text-xs text-ink-muted">{{ cloud.cache.id }}</p>
                </div>
                <span v-if="cloud.cache" :class="toneBadgeClass(dataTone(cloud.cache.status))">{{ cloud.cache.status }}</span>
              </div>
              <p v-if="cloud.cache_error" class="text-sm text-danger">{{ cloud.cache_error }}</p>
              <dl v-if="cloud.cache" class="grid grid-cols-2 gap-x-4 gap-y-1 text-sm">
                <dt class="text-ink-muted">Node type</dt>
                <dd class="font-mono text-xs">{{ cloud.cache.node_type || 'Unknown' }}</dd>
                <dt class="text-ink-muted">Multi-AZ</dt>
                <dd>{{ cloud.cache.multi_az ? 'Yes' : 'No' }}</dd>
                <dt class="text-ink-muted">Failover</dt>
                <dd>{{ cloud.cache.automatic_failover === 'enabled' ? 'Automatic' : cloud.cache.automatic_failover || 'Unknown' }}</dd>
                <template v-for="n in cloud.cache.nodes" :key="n.id">
                  <dt class="font-mono text-xs text-ink-muted">{{ n.id }}</dt>
                  <dd>{{ n.role ? n.role.charAt(0).toUpperCase() + n.role.slice(1) : 'Node' }}<template v-if="n.zone">, {{ n.zone }}</template></dd>
                </template>
              </dl>
              <RouterLink to="/admin/infrastructure/resources/cache" class="infra-resource-link block text-sm text-accent hover:text-ink-bright">
                Its CPU, memory, connections and network over time
              </RouterLink>
            </article>
          </div>
        </section>
      </template>

      <!-- The lobby instances (lobby_instances). -->
      <section class="space-y-4" aria-labelledby="infra-lobbies">
        <h2 id="infra-lobbies" class="text-xl font-semibold text-ink">Lobby instances</h2>
        <p class="text-sm text-ink-muted">
          One per lobby task; a draining one takes no new client. Their sessions are on
          <RouterLink :to="{ path: '/admin/novaworld', hash: '#sessions' }" class="text-accent hover:text-ink-bright">NovaWorld's status and sessions</RouterLink>.
        </p>
        <div class="relative overflow-x-auto rounded-panel border border-border bg-panel">
          <table class="min-w-full divide-y divide-border">
            <caption class="sr-only">Lobby instances</caption>
            <thead>
              <tr :class="headRow">
                <th scope="col" class="px-4 py-3">Instance</th>
                <th scope="col" class="px-4 py-3">Address</th>
                <th scope="col" class="px-4 py-3">State</th>
                <th scope="col" class="px-4 py-3">Sessions</th>
                <th scope="col" class="px-4 py-3">Started</th>
                <th scope="col" class="px-4 py-3">Last seen</th>
              </tr>
            </thead>
            <tbody class="divide-y divide-border text-sm text-ink">
              <tr v-if="overview.lobbies.length === 0">
                <td colspan="6" class="px-4 py-6 text-center text-ink-muted">No lobby instance is registered.</td>
              </tr>
              <tr v-for="l in overview.lobbies" :key="l.id" class="infra-lobby-row hover:bg-hover">
                <th scope="row" class="whitespace-nowrap px-4 py-3 text-left font-mono text-xs font-normal">{{ l.id }}</th>
                <td class="whitespace-nowrap px-4 py-3 font-mono text-xs">{{ l.endpoint }}</td>
                <td class="whitespace-nowrap px-4 py-3">
                  <span :class="toneBadgeClass(lobbyTone(l))">{{ lobbyLabel(l) }}</span>
                </td>
                <td class="whitespace-nowrap px-4 py-3">
                  {{ countLabel(l.sessions, 'session') }}<span v-if="l.listings" class="block text-xs text-ink-muted">{{ countLabel(l.listings, 'listed server') }}</span>
                </td>
                <td class="whitespace-nowrap px-4 py-3 text-xs text-ink-muted"><RelativeTime :value="l.started_at" :now="now" /></td>
                <td class="whitespace-nowrap px-4 py-3 text-xs" :class="l.live ? 'text-ink-muted' : 'text-warn'">
                  <RelativeTime :value="l.last_seen_at" :now="now" />
                </td>
              </tr>
            </tbody>
          </table>
        </div>
      </section>

      <!-- The fleet program's leader (fleet_leader). -->
      <section class="space-y-4" aria-labelledby="infra-fleet">
        <h2 id="infra-fleet" class="text-xl font-semibold text-ink">Fleet leader</h2>
        <dl class="infra-fleet grid grid-cols-1 gap-x-6 gap-y-1 rounded-panel border border-border bg-panel p-4 text-sm sm:grid-cols-3">
          <div>
            <dt class="text-xs text-ink-muted">Leader</dt>
            <dd class="font-mono">{{ overview.fleet.leader || 'None elected' }}</dd>
          </div>
          <div>
            <dt class="text-xs text-ink-muted">Elected</dt>
            <dd>
              <RelativeTime v-if="overview.fleet.elected_at" :value="overview.fleet.elected_at" :now="now" />
              <template v-else>Never</template>
            </dd>
          </div>
          <div>
            <dt class="text-xs text-ink-muted">Election</dt>
            <dd>{{ overview.fleet.generation === 0 ? 'None yet' : `The ${ordinal(overview.fleet.generation)}` }}</dd>
          </div>
        </dl>
      </section>

      <!-- The fleet's machines (fleet_machines): their own page is the Fleet's. -->
      <section class="space-y-2" aria-labelledby="infra-machines">
        <h2 id="infra-machines" class="text-xl font-semibold text-ink">Fleet machines</h2>
        <p class="infra-machines-summary text-sm text-ink">
          {{ machinesSummary }}
          <RouterLink to="/admin/fleet" class="text-accent hover:text-ink-bright">The fleet: machines, servers and their moves</RouterLink>
        </p>
        <p v-if="ec2Gone > 0" class="text-sm text-warn">EC2 no longer runs {{ countLabel(ec2Gone, 'live machine') }} of them.</p>
        <p v-if="cloud?.machines_error" class="text-sm text-danger">EC2 could not be read: {{ cloud.machines_error }}</p>
      </section>
    </template>
  </div>
</template>

<script setup lang="ts">
// /admin/infrastructure: the service's hub (GET /api/admin/infrastructure and
// GET /api/admin/programs). Above, at a glance: the programs steady, the tasks
// running, the load balancer's healthy targets, the release, the live lobby
// instances and the fleet's machines, and the warnings to look at first.
// Below: a card per program (ProgramCard: its ECS service with its tasks,
// stopped tasks and events, what its tasks use now and its tasks gone in 24
// hours), each opening its own page (usage per task, its logs; hostd's daemon
// and the fleet cluster are the Fleet page's); the target groups, the database and the
// cache (from AWS, which web reads at most every 20 seconds), each opening its
// charts; then the lobby instances, the fleet leader and one line on the
// machines (from the database), whose page is the Fleet's. On the local stack
// AWS is not read, and the page says so rather than showing anything made up;
// the programs' own samples still show. Polled every 30 seconds.
import AdminPageHeader from '../../components/AdminPageHeader.vue';
import { computed, ref } from 'vue';
import { RouterLink } from 'vue-router';
import ProgramCard from '../../components/ProgramCard.vue';
import RelativeTime from '../../components/RelativeTime.vue';
import StatTile from '../../components/charts/StatTile.vue';
import { fetchInfrastructure } from '../../api/infra';
import { fetchPrograms } from '../../api/usage';
import { usePolling } from '../../composables/usePolling';
import { keepInView } from '../../utils/keepInView';
import type { InfraCloudRead, InfraLobby, InfraOverview, InfraService, InfraTask } from '../../types/infra';
import type { ProgramUsage, ProgramsResponse } from '../../types/usage';
import {
  countLabel,
  infraWarnings,
  listenerLabel,
  offRelease,
  serviceState,
  targetLabel,
  targetTotals,
  taskStatusLabel,
  taskTotals,
  toneBadgeClass,
  type Tone,
} from '../../utils/infraView';

const POLL_MS = 30_000;

const headRow = 'bg-raised text-left text-xs font-semibold uppercase tracking-wider text-ink-muted';

const root = ref<HTMLElement | null>(null);
const overview = ref<InfraOverview | null>(null);
const programs = ref<ProgramsResponse | null>(null);
const programsError = ref('');
const loadedAt = ref<number | null>(null);
const now = ref(Date.now());
const error = ref('');
const pollTrouble = ref(false);

// AWS's sections, when they were read.
const cloud = computed<InfraCloudRead | null>(() => (overview.value?.cloud.configured ? overview.value.cloud : null));
// AWS's sections, when they were read whole.
const cloudRead = computed<InfraCloudRead | null>(() => (cloud.value && !cloud.value.error ? cloud.value : null));
const warnings = computed(() => (overview.value ? infraWarnings(overview.value) : []));

// The programs: the service's (the samples' list, in its order), then any
// other service ECS runs in the cluster.
const programNames = computed(() => {
  const names = (programs.value?.programs ?? []).map((p) => p.name);
  for (const s of cloudRead.value?.services ?? []) if (!names.includes(s.name)) names.push(s.name);
  return names;
});
function serviceOf(name: string): InfraService | null {
  return cloudRead.value?.services.find((s) => s.name === name) ?? null;
}
function usageOf(name: string): ProgramUsage | null {
  return programs.value?.programs.find((p) => p.name === name) ?? null;
}
// The cloud's tiles say why they are empty.
const noCloud = computed(() => {
  if (!overview.value) return '';
  if (!overview.value.cloud.configured) return 'Not read on this stack';
  return overview.value.cloud.error ? 'AWS could not be read' : '';
});

const programsTile = computed(() => {
  const c = cloud.value;
  if (!c || c.error) return { value: 'Unknown', detail: noCloud.value };
  const steady = c.services.filter((s) => serviceState(s).label === 'Steady').length;
  const others = c.services.filter((s) => serviceState(s).label !== 'Steady');
  return {
    value: `${steady} of ${c.services.length} steady`,
    detail: others.length ? others.map((s) => `${s.name} ${serviceState(s).label.toLowerCase()}`).join(', ') : 'Every service at its desired count',
  };
});

const tasksTile = computed(() => {
  const c = cloud.value;
  if (!c || c.error) return { value: 'Unknown', detail: noCloud.value };
  const t = taskTotals(c);
  const parts = [t.pending ? `${t.pending} starting` : 'None starting'];
  if (t.unhealthy) parts.push(`${t.unhealthy} unhealthy`);
  return { value: `${t.running} of ${t.desired}`, detail: parts.join(', ') };
});

const targetsTile = computed(() => {
  const c = cloud.value;
  if (!c || c.error) return { value: 'Unknown', detail: noCloud.value };
  const t = targetTotals(c.target_groups);
  const failed = c.target_groups.filter((g) => g.error).length;
  return {
    value: `${t.healthy} of ${t.total} healthy`,
    detail: failed ? `${countLabel(failed, 'target group')} could not be read` : `Across ${countLabel(c.target_groups.length, 'listener')}`,
  };
});

const releaseTile = computed(() => {
  const c = cloud.value;
  if (!c || c.error) return { value: 'Unknown', detail: noCloud.value };
  if (c.release_error) return { value: 'Unknown', detail: 'The release could not be read' };
  if (c.release_none || !c.release) return { value: 'None yet', detail: 'No app deploy has recorded a release' };
  const off = offRelease(c);
  const value = c.release.revision ? c.release.revision.slice(0, 7) : c.release.id;
  const runs = off.length ? `${off.map((s) => s.name).join(', ')} ${off.length === 1 ? 'runs' : 'run'} another revision` : 'Every program runs it';
  return { value, detail: `${c.release.id}. ${runs}` };
});

const lobbiesTile = computed(() => {
  const lobbies = overview.value?.lobbies ?? [];
  const live = lobbies.filter((l) => l.live);
  const taking = live.filter((l) => l.state === 'active').length;
  const sessions = lobbies.reduce((n, l) => n + l.sessions, 0);
  return {
    value: `${live.length} live`,
    detail: lobbies.length ? `${taking} taking clients, ${countLabel(sessions, 'session')}` : 'None registered',
  };
});

const machinesTile = computed(() => {
  const o = overview.value;
  if (!o) return { value: '', detail: '' };
  const live = o.machines.filter((m) => m.state !== 'ended' && m.state !== 'failed');
  const used = live.reduce((n, m) => n + m.slots_used, 0);
  const slots = live.reduce((n, m) => n + m.slots, 0);
  if (!o.hosting.enabled && live.length === 0) return { value: '0', detail: 'Hosting is off' };
  return { value: String(live.length), detail: live.length ? `${used} of ${slots} slots used` : 'No machine runs' };
});

// The live machines in one line: their kinds and their slots.
const liveMachines = computed(() => (overview.value?.machines ?? []).filter((m) => m.state !== 'ended' && m.state !== 'failed'));
const machinesSummary = computed(() => {
  const o = overview.value;
  if (!o) return '';
  const live = liveMachines.value;
  if (live.length === 0) return o.hosting.enabled ? 'No machine runs.' : 'Hosting is off on this service.';
  const shared = live.filter((m) => m.kind === 'shared').length;
  const used = live.reduce((n, m) => n + m.slots_used, 0);
  const slots = live.reduce((n, m) => n + m.slots, 0);
  return `${countLabel(live.length, 'machine')} live (${shared} shared, ${live.length - shared} own), ${used} of ${slots} slots in use.`;
});
// Live machines EC2 lists as no longer running.
const ec2Gone = computed(() => liveMachines.value.filter((m) => m.ec2_state !== null && m.ec2_state !== 'running' && m.ec2_state !== 'pending').length);

function stoppedReason(t: InfraTask): string {
  const exits = t.containers
    .filter((c) => c.exit_code !== null)
    .map((c) => `${c.name} exited with code ${c.exit_code}`);
  return [t.stopped_reason || 'No reason given', ...exits].join('; ');
}

function dataTone(status: string): Tone {
  if (status === 'available') return 'ok';
  if (status === 'failed' || status === 'incompatible-parameters' || status === 'storage-full' || status === 'inaccessible-encryption-credentials') {
    return 'bad';
  }
  return 'warn';
}

function lobbyTone(l: InfraLobby): Tone {
  if (!l.live) return 'bad';
  return l.state === 'active' ? 'ok' : 'warn';
}

function lobbyLabel(l: InfraLobby): string {
  if (!l.live) return 'Not seen';
  return l.state === 'active' ? 'Taking clients' : 'Draining';
}

// "3rd": an election's number.
function ordinal(n: number): string {
  const tens = n % 100;
  if (tens >= 11 && tens <= 13) return `${n}th`;
  const ones = n % 10;
  return `${n}${ones === 1 ? 'st' : ones === 2 ? 'nd' : ones === 3 ? 'rd' : 'th'}`;
}

const poll = usePolling({
  async read(signal, background) {
    const [result, used] = await Promise.all([fetchInfrastructure(signal), fetchPrograms(signal)]);
    if (signal.aborted) return false;
    now.value = Date.now();
    // What the programs use is a section of its own: its failure leaves the rest.
    if (used.ok) {
      programs.value = used.value;
      programsError.value = '';
    } else {
      programsError.value = used.error.message;
    }
    if (result.ok) {
      await keepInView(() => {
        overview.value = result.value;
      }, root.value);
      loadedAt.value = now.value;
      pollTrouble.value = false;
      error.value = '';
      return true;
    }
    if (background && overview.value !== null) pollTrouble.value = true;
    else error.value = result.error.message;
    return false;
  },
  next: () => POLL_MS,
});
</script>
