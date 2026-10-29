<template>
  <div ref="root" class="space-y-8">
    <PageHeader title="System" :tabs="SYSTEM_TABS" refreshable :loaded-at="loadedAt" :now="now" :loading="poll.loading.value" @refresh="poll.refresh">
      <template v-if="cloudRead" #meta>
        <span class="font-mono">{{ cloudRead.cluster }}</span>
        <span>{{ cloudRead.region }}</span>
        <span>Read from AWS <RelativeTime :value="cloudRead.read_at" :now="now" /></span>
      </template>
    </PageHeader>

    <LiveNotices :error="error" :poll-trouble="pollTrouble" />
    <p v-if="overview === null && poll.loading.value" class="text-sm text-ink-muted">Loading…</p>

    <template v-if="overview">
      <Notice v-if="!overview.cloud.configured" class="infra-unavailable" :title="overview.cloud.unavailable">AWS is not read on this stack.</Notice>
      <Notice v-else-if="cloud?.error" tone="bad" class="infra-cloud-error">AWS could not be read: {{ cloud.error }}</Notice>

      <RunningBuilds :overview="overview" :programs="programs" :names="programNames" :now="now" />

      <ul v-if="warnings.length" class="infra-warnings space-y-2">
        <li v-for="w in warnings" :key="w" class="notice notice-warn">{{ w }}</li>
      </ul>

      <dl class="infra-tiles grid grid-cols-1 gap-3 min-[420px]:grid-cols-2 lg:grid-cols-3">
        <StatTile label="Programs" :value="programsTile.value" :detail="programsTile.detail" />
        <StatTile label="Tasks running" :value="tasksTile.value" :detail="tasksTile.detail" />
        <StatTile label="Load balancer targets" :value="targetsTile.value" :detail="targetsTile.detail" />
        <StatTile label="Lobby instances" to="/admin/system" :value="lobbiesTile.value" :detail="lobbiesTile.detail" />
        <StatTile label="Machines" to="/admin/servers/machines" :value="machinesTile.value" :detail="machinesTile.detail" :tone="machinesTile.tone" />
      </dl>

      <section v-if="programNames.length" class="space-y-3" aria-labelledby="infra-programs">
        <h2 id="infra-programs" class="section-heading">Programs</h2>
        <template v-if="cloudRead">
          <p v-if="cloudRead.services_error" class="text-sm text-danger">The services could not be read: {{ cloudRead.services_error }}</p>
          <p v-if="cloudRead.tasks_error" class="text-sm text-danger">The tasks could not be read: {{ cloudRead.tasks_error }}</p>
        </template>
        <p v-if="programsError" class="infra-programs-error text-sm text-danger">Program usage could not be read: {{ programsError }}</p>
        <div class="grid gap-4 xl:grid-cols-2">
          <ProgramCard
            v-for="name in programNames"
            :key="name"
            :name="name"
            :service="serviceOf(name)"
            :usage="usageOf(name)"
            :release="cloudRead?.release ?? null"
            :cloud-read="cloudRead !== null"
            :now="now"
            :to="`/admin/programs/${name}`"
          />
        </div>

        <div v-if="cloudRead && (cloudRead.one_off_tasks.length || cloudRead.one_off_stopped_tasks.length)" class="infra-one-off card p-4">
          <h3 class="text-sm font-semibold text-ink">One-off tasks</h3>
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
              <span><template v-if="t.stopped_at">Stopped <RelativeTime :value="t.stopped_at" :now="now" />: </template>{{ stoppedReason(t) }}</span>
            </li>
          </ul>
        </div>
      </section>

      <template v-if="cloud && !cloud.error">
        <section class="space-y-3" aria-labelledby="infra-targets">
          <div class="flex items-baseline justify-between gap-3">
            <h2 id="infra-targets" class="section-heading">Load balancer</h2>
            <RouterLink to="/admin/resources/nlb" class="infra-resource-link link text-sm">Charts<span class="sr-only">: load balancer</span></RouterLink>
          </div>
          <div class="table-wrap">
            <table class="data-table">
              <caption class="sr-only">Load balancer targets</caption>
              <thead>
                <tr>
                  <th scope="col">Listener</th>
                  <th scope="col">Target</th>
                  <th scope="col">Zone</th>
                  <th scope="col">Health</th>
                  <th scope="col">Why</th>
                </tr>
              </thead>
              <tbody>
                <TableState v-if="cloud.target_groups.length === 0" :colspan="5" :rows="cloud.target_groups" empty="No target groups." />
                <template v-for="g in cloud.target_groups" :key="g.key">
                  <tr v-if="g.error" class="infra-target-row">
                    <th scope="row" class="whitespace-nowrap font-normal">{{ listenerLabel(g.key) }}</th>
                    <td colspan="4" class="text-danger">Could not be read: {{ g.error }}</td>
                  </tr>
                  <tr v-else-if="g.targets.length === 0" class="infra-target-row">
                    <th scope="row" class="whitespace-nowrap font-normal">{{ listenerLabel(g.key) }}</th>
                    <td colspan="4" class="text-warn">No target</td>
                  </tr>
                  <tr v-for="t in g.error ? [] : g.targets" :key="`${g.key} ${t.id}:${t.port}`" class="infra-target-row">
                    <th scope="row" class="whitespace-nowrap font-normal">
                      {{ listenerLabel(g.key) }}
                      <span class="block font-mono text-xs text-ink-muted">{{ g.name }}</span>
                    </th>
                    <td class="whitespace-nowrap font-mono text-xs">{{ t.id }}:{{ t.port }}</td>
                    <td class="whitespace-nowrap text-xs">{{ t.zone || 'Unknown' }}</td>
                    <td class="whitespace-nowrap"><Badge :tone="targetLabel(t.state).tone">{{ targetLabel(t.state).label }}</Badge></td>
                    <td class="text-xs text-ink-muted">
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

        <section class="space-y-3" aria-labelledby="infra-data">
          <h2 id="infra-data" class="section-heading">Database and cache</h2>
          <div class="grid gap-4 md:grid-cols-2">
            <article class="infra-database card space-y-2 p-4" aria-labelledby="infra-database-heading">
              <div class="flex items-start justify-between gap-3">
                <div>
                  <h3 id="infra-database-heading" class="font-semibold text-ink">
                    <RouterLink to="/admin/resources/rds" class="infra-resource-link entity-link">PostgreSQL (RDS)</RouterLink>
                  </h3>
                  <p v-if="cloud.database" class="font-mono text-xs text-ink-muted">{{ cloud.database.identifier }}</p>
                </div>
                <Badge v-if="cloud.database" :tone="dataTone(cloud.database.status)">{{ cloud.database.status }}</Badge>
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
                <dd>{{ cloud.database.zone || 'Unknown' }}<template v-if="cloud.database.secondary_zone">, standby in {{ cloud.database.secondary_zone }}</template></dd>
                <dt class="text-ink-muted">Storage</dt>
                <dd>{{ cloud.database.storage_gb }} GiB{{ cloud.database.storage_type ? ` ${cloud.database.storage_type}` : '' }}</dd>
              </dl>
            </article>
            <article class="infra-cache card space-y-2 p-4" aria-labelledby="infra-cache-heading">
              <div class="flex items-start justify-between gap-3">
                <div>
                  <h3 id="infra-cache-heading" class="font-semibold text-ink">
                    <RouterLink to="/admin/resources/cache" class="infra-resource-link entity-link">Valkey (ElastiCache)</RouterLink>
                  </h3>
                  <p v-if="cloud.cache" class="font-mono text-xs text-ink-muted">{{ cloud.cache.id }}</p>
                </div>
                <Badge v-if="cloud.cache" :tone="dataTone(cloud.cache.status)">{{ cloud.cache.status }}</Badge>
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
            </article>
          </div>
        </section>
      </template>

      <section class="space-y-3" aria-labelledby="infra-fleet-heading">
        <h2 id="infra-fleet-heading" class="section-heading">Fleet leader</h2>
        <dl class="infra-fleet card grid grid-cols-1 gap-x-6 gap-y-1 p-4 text-sm sm:grid-cols-3">
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
    </template>
  </div>
</template>

<script setup lang="ts">
// /admin/system/infrastructure: the deployment as it runs, from AWS and the
// database: first the release and the build each part runs (RunningBuilds),
// then the warnings, tiles at a glance, a card per program (each to its page),
// the load balancer's targets, the database and the cache (each to its charts)
// and the fleet leader. On the local stack AWS is not read. Polled every 30
// seconds.
import { computed, ref } from 'vue';
import { RouterLink } from 'vue-router';
import ProgramCard from '../../../components/ProgramCard.vue';
import RelativeTime from '../../../components/RelativeTime.vue';
import RunningBuilds from '../../../components/RunningBuilds.vue';
import StatTile from '../../../components/charts/StatTile.vue';
import Badge from '../../../components/ui/Badge.vue';
import LiveNotices from '../../../components/ui/LiveNotices.vue';
import Notice from '../../../components/ui/Notice.vue';
import PageHeader from '../../../components/ui/PageHeader.vue';
import TableState from '../../../components/ui/TableState.vue';
import { fetchInfrastructure } from '../../../api/infra';
import { fetchPrograms } from '../../../api/usage';
import { usePolling } from '../../../composables/usePolling';
import { keepInView } from '../../../utils/keepInView';
import type { InfraCloudRead, InfraOverview, InfraService, InfraTask } from '../../../types/infra';
import type { ProgramUsage, ProgramsResponse } from '../../../types/usage';
import { SYSTEM_TABS } from '../../../utils/adminNav';
import { countLabel, infraWarnings, listenerLabel, serviceState, targetLabel, targetTotals, taskStatusLabel, taskTotals } from '../../../utils/infraView';
import type { Tone } from '../../../utils/tone';

const POLL_MS = 30_000;

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
    detail: others.length ? others.map((s) => `${s.name} ${serviceState(s).label.toLowerCase()}`).join(', ') : undefined,
  };
});

const tasksTile = computed(() => {
  const c = cloud.value;
  if (!c || c.error) return { value: 'Unknown', detail: noCloud.value };
  const t = taskTotals(c);
  const parts = [...(t.pending ? [`${t.pending} starting`] : []), ...(t.unhealthy ? [`${t.unhealthy} unhealthy`] : [])];
  return { value: `${t.running} of ${t.desired}`, detail: parts.length ? parts.join(', ') : undefined };
});

const targetsTile = computed(() => {
  const c = cloud.value;
  if (!c || c.error) return { value: 'Unknown', detail: noCloud.value };
  const t = targetTotals(c.target_groups);
  const failed = c.target_groups.filter((g) => g.error).length;
  return {
    value: `${t.healthy} of ${t.total} healthy`,
    detail: failed ? `${countLabel(failed, 'target group')} not read` : countLabel(c.target_groups.length, 'listener'),
  };
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

// The fleet's live machines: their count, their slots, and what EC2 says.
const machinesTile = computed<{ value: string; detail?: string; tone?: Tone }>(() => {
  const o = overview.value;
  if (!o) return { value: '' };
  const live = o.machines.filter((m) => m.state !== 'ended' && m.state !== 'failed');
  const gone = live.filter((m) => m.ec2_state !== null && m.ec2_state !== 'running' && m.ec2_state !== 'pending').length;
  if (cloud.value?.machines_error) return { value: String(live.length), detail: `EC2 not read: ${cloud.value.machines_error}`, tone: 'warn' };
  if (gone > 0) return { value: String(live.length), detail: `EC2 no longer runs ${gone}`, tone: 'warn' };
  if (!o.hosting.enabled && live.length === 0) return { value: '0', detail: 'Hosting is off' };
  const shared = live.filter((m) => m.kind === 'shared').length;
  const used = live.reduce((n, m) => n + m.slots_used, 0);
  const slots = live.reduce((n, m) => n + m.slots, 0);
  return { value: String(live.length), detail: live.length ? `${shared} shared, ${live.length - shared} dedicated, ${used} of ${slots} slots used` : undefined };
});

function stoppedReason(t: InfraTask): string {
  const exits = t.containers.filter((c) => c.exit_code !== null).map((c) => `${c.name} exited with code ${c.exit_code}`);
  return [t.stopped_reason || 'No reason given', ...exits].join('; ');
}

function dataTone(status: string): Tone {
  if (status === 'available') return 'ok';
  if (status === 'failed' || status === 'incompatible-parameters' || status === 'storage-full' || status === 'inaccessible-encryption-credentials') {
    return 'bad';
  }
  return 'warn';
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
