<template>
  <div class="space-y-10">
    <header class="flex flex-col gap-4 sm:flex-row sm:items-center sm:justify-between">
      <div>
        <h1 class="text-3xl font-semibold text-ink">Infrastructure</h1>
        <p class="text-sm text-ink-muted">
          The deployment as it runs: each program's ECS service and its tasks, the load balancer's targets, the
          database and the cache, the lobby instances, the controller and the fleet's machines.
        </p>
      </div>
      <div class="flex items-center gap-3">
        <p v-if="loadedAt" class="text-xs text-ink-muted">Updated <RelativeTime :value="loadedAt" :now="now" /></p>
        <button type="button" :class="secondaryButton" :disabled="poll.loading.value" @click="poll.refresh">
          {{ poll.loading.value ? 'Refreshing…' : 'Refresh' }}
        </button>
      </div>
    </header>

    <!-- A live region, always in the page; the box below is its visible copy. -->
    <div role="alert" class="sr-only">{{ error }}</div>
    <div v-if="error" aria-hidden="true" class="rounded-panel border border-danger/40 bg-danger/10 p-4 text-sm text-danger">{{ error }}</div>
    <p v-if="pollTrouble" class="text-xs text-warn">The last refresh did not get through; this page keeps trying.</p>
    <div v-if="overview === null && poll.loading.value" class="flex h-32 items-center justify-center text-ink-muted">
      Loading the deployment…
    </div>

    <template v-if="overview">
      <p v-if="!overview.cloud.configured" class="infra-unavailable rounded-panel border border-border bg-panel p-4 text-sm text-ink-muted">
        {{ overview.cloud.unavailable }}. The lobby instances, the controller and the machines below come from the
        database.
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

      <template v-if="cloud && !cloud.error">
        <!-- The programs: an ECS service each. -->
        <section class="space-y-4" aria-labelledby="infra-programs">
          <div class="flex flex-col gap-1 sm:flex-row sm:items-baseline sm:justify-between">
            <h2 id="infra-programs" class="text-xl font-semibold text-ink">Programs</h2>
            <p class="text-xs text-ink-muted">
              Cluster <span class="font-mono text-ink">{{ cloud.cluster }}</span> in {{ cloud.region }}, read from AWS
              <RelativeTime :value="cloud.read_at" :now="now" />
            </p>
          </div>
          <p v-if="cloud.services_error" class="text-sm text-danger">The services could not be read: {{ cloud.services_error }}</p>
          <p v-if="cloud.tasks_error" class="text-sm text-danger">The tasks could not be read: {{ cloud.tasks_error }}</p>
          <p v-if="cloud.release_error" class="text-sm text-danger">The release could not be read: {{ cloud.release_error }}</p>
          <div class="grid gap-4 xl:grid-cols-2">
            <article
              v-for="s in cloud.services"
              :key="s.name"
              class="infra-service min-w-0 space-y-3 rounded-panel border border-border bg-panel p-4"
              :aria-labelledby="`infra-service-${s.name}`"
            >
              <div class="flex items-start justify-between gap-3">
                <div class="min-w-0">
                  <h3 :id="`infra-service-${s.name}`" class="text-lg font-semibold text-ink">{{ s.name }}</h3>
                  <p class="break-all font-mono text-xs text-ink-muted">{{ s.task_definition }}</p>
                  <p v-if="s.on_release !== null" class="text-xs" :class="s.on_release ? 'text-ink-muted' : 'text-warn'">
                    {{ s.on_release ? "The release's revision" : "Not the release's revision" }}
                  </p>
                </div>
                <div class="flex shrink-0 flex-col items-end gap-1">
                  <span :class="toneBadgeClass(serviceState(s).tone)">{{ serviceState(s).label }}</span>
                  <span class="whitespace-nowrap text-sm text-ink">{{ s.running }} of {{ s.desired }} running</span>
                  <span v-if="s.pending > 0" class="text-xs text-warn">{{ s.pending }} starting</span>
                </div>
              </div>

              <ul v-if="showDeployments(s)" class="space-y-1 text-xs text-ink-muted">
                <li v-for="d in s.deployments" :key="d.id" class="infra-deployment">
                  <span class="font-medium text-ink">{{ d.status === 'PRIMARY' ? 'New' : 'Old' }}</span> <span class="font-mono">{{
                    d.task_definition
                  }}</span>: {{ d.running }} of {{ d.desired }} running<template
                    v-if="d.failed > 0"
                    >, <span class="text-danger">{{ countLabel(d.failed, 'task') }} failed</span></template
                  >, rollout {{ rolloutLabel(d.rollout_state).toLowerCase()
                  }}<template v-if="d.created_at">, began <RelativeTime :value="d.created_at" :now="now" /></template>
                </li>
              </ul>
              <p v-else-if="primaryDeployment(s)?.created_at" class="text-xs text-ink-muted">
                Deployed <RelativeTime :value="primaryDeployment(s)!.created_at!" :now="now" />, rollout
                {{ rolloutLabel(primaryDeployment(s)!.rollout_state).toLowerCase() }}
              </p>

              <ul v-if="s.tasks.length" class="divide-y divide-border border-t border-border">
                <li v-for="t in s.tasks" :key="t.id" class="infra-task space-y-1 py-2 text-sm">
                  <div class="flex flex-wrap items-center gap-x-3 gap-y-1">
                    <span class="font-mono text-ink" :title="`Task ${t.id}`">{{ t.id.slice(0, 8) }}</span>
                    <span :class="toneBadgeClass(healthLabel(t.health).tone)">{{ healthLabel(t.health).label }}</span>
                    <span v-if="t.last_status !== 'RUNNING'" class="text-xs text-warn">{{ taskStatusLabel(t.last_status) }}</span>
                    <span v-if="t.on_release === false" class="text-xs text-warn">Revision {{ revisionOf(t.task_definition) }}</span>
                  </div>
                  <p class="text-xs text-ink-muted">
                    <template v-if="t.started_at">Started <RelativeTime :value="t.started_at" :now="now" /></template>
                    <template v-else-if="t.created_at">Created <RelativeTime :value="t.created_at" :now="now" />, not started</template>
                    <template v-if="t.zone">, {{ t.zone }}</template>
                    <template v-if="t.private_ip">, <span class="font-mono">{{ t.private_ip }}</span></template>
                    <template v-if="taskSize(t)">, {{ taskSize(t) }}</template>
                  </p>
                  <p v-for="c in t.containers" :key="c.name" class="break-all text-xs text-ink-muted">
                    <span class="text-ink">{{ c.name }}</span>:
                    <span class="font-mono" :title="c.image_digest ? `${c.image} (pulled ${c.image_digest})` : c.image">{{ shortImage(c.image) }}</span>
                    <span v-if="c.health === 'UNHEALTHY'" class="text-danger">{{ ' unhealthy' }}</span>
                  </p>
                </li>
              </ul>
              <p v-else class="border-t border-border pt-2 text-sm text-ink-muted">No task runs.</p>

              <div v-if="s.stopped_tasks.length" class="space-y-1">
                <h4 class="text-xs font-semibold uppercase tracking-wider text-ink-muted">Recently stopped</h4>
                <ul class="space-y-1">
                  <li v-for="t in s.stopped_tasks" :key="t.id" class="infra-stopped-task text-xs text-ink-muted">
                    <span class="font-mono text-ink" :title="`Task ${t.id}`">{{ t.id.slice(0, 8) }}</span>
                    <template v-if="t.stopped_at"> stopped <RelativeTime :value="t.stopped_at" :now="now" /></template>:
                    {{ stoppedReason(t) }}
                  </li>
                </ul>
              </div>

              <details v-if="s.events.length" class="text-xs">
                <summary class="cursor-pointer text-accent hover:text-ink-bright">Recent events ({{ s.events.length }})</summary>
                <ul class="mt-2 space-y-1 text-ink-muted">
                  <li v-for="(e, i) in s.events" :key="i" class="infra-event">
                    <RelativeTime v-if="e.at" :value="e.at" :now="now" class="whitespace-nowrap" />: {{ e.message }}
                  </li>
                </ul>
              </details>
            </article>
          </div>

          <div v-if="cloud.one_off_tasks.length || cloud.one_off_stopped_tasks.length" class="infra-one-off rounded-panel border border-border bg-panel p-4">
            <h3 class="text-sm font-semibold text-ink">One-off tasks</h3>
            <p class="text-xs text-ink-muted">The nightly backup, and the toolbox's database tasks.</p>
            <ul class="mt-2 space-y-1 text-sm">
              <li v-for="t in cloud.one_off_tasks" :key="t.id" class="flex flex-wrap gap-x-3">
                <span class="font-mono" :title="`Task ${t.id}`">{{ t.id.slice(0, 8) }}</span>
                <span>{{ t.group.replace(/^family:/, '') }}</span>
                <span class="text-xs text-ink-muted">
                  {{ taskStatusLabel(t.last_status) }}<template v-if="t.started_at">, started <RelativeTime :value="t.started_at" :now="now" /></template>
                </span>
              </li>
              <li v-for="t in cloud.one_off_stopped_tasks" :key="t.id" class="flex flex-wrap gap-x-3 text-xs text-ink-muted">
                <span class="font-mono text-ink" :title="`Task ${t.id}`">{{ t.id.slice(0, 8) }}</span>
                <span>{{ t.group.replace(/^family:/, '') }}</span>
                <span>
                  <template v-if="t.stopped_at">Stopped <RelativeTime :value="t.stopped_at" :now="now" />: </template>{{ stoppedReason(t) }}
                </span>
              </li>
            </ul>
          </div>
        </section>

        <!-- The load balancer's target groups. -->
        <section class="space-y-4" aria-labelledby="infra-targets">
          <h2 id="infra-targets" class="text-xl font-semibold text-ink">Load balancer</h2>
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
            </article>
          </div>
        </section>
      </template>

      <!-- The lobby instances (lobby_instances). -->
      <section class="space-y-4" aria-labelledby="infra-lobbies">
        <h2 id="infra-lobbies" class="text-xl font-semibold text-ink">Lobby instances</h2>
        <p class="text-sm text-ink-muted">
          One per lobby task, each moving its row every few seconds. Every instance announces the one lobby address; a
          draining one takes no new client. Their sessions are on
          <RouterLink to="/admin/sessions" class="text-accent hover:text-ink-bright">NovaWorld sessions</RouterLink>.
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

      <!-- The controller (controller_leader). -->
      <section class="space-y-4" aria-labelledby="infra-controller">
        <h2 id="infra-controller" class="text-xl font-semibold text-ink">Controller</h2>
        <dl class="infra-controller grid grid-cols-1 gap-x-6 gap-y-1 rounded-panel border border-border bg-panel p-4 text-sm sm:grid-cols-3">
          <div>
            <dt class="text-xs text-ink-muted">Leader</dt>
            <dd class="font-mono">{{ overview.controller.leader || 'None elected' }}</dd>
          </div>
          <div>
            <dt class="text-xs text-ink-muted">Elected</dt>
            <dd>
              <RelativeTime v-if="overview.controller.elected_at" :value="overview.controller.elected_at" :now="now" />
              <template v-else>Never</template>
            </dd>
          </div>
          <div>
            <dt class="text-xs text-ink-muted">Election</dt>
            <dd>{{ overview.controller.generation === 0 ? 'None yet' : `The ${ordinal(overview.controller.generation)}` }}</dd>
          </div>
        </dl>
      </section>

      <!-- The fleet's machines (fleet_machines). -->
      <section class="space-y-4" aria-labelledby="infra-machines">
        <div class="flex flex-col gap-1 sm:flex-row sm:items-baseline sm:justify-between">
          <h2 id="infra-machines" class="text-xl font-semibold text-ink">Fleet machines</h2>
          <RouterLink to="/admin/fleet" class="text-sm text-accent hover:text-ink-bright">The fleet: machines, servers and their moves</RouterLink>
        </div>
        <p class="text-sm text-ink-muted">
          The machines hosted servers run on, live then the newest ended.
          <template v-if="!overview.hosting.enabled">Hosting is off on this service.</template>
        </p>
        <p v-if="cloud?.machines_error" class="text-sm text-danger">EC2 could not be read: {{ cloud.machines_error }}</p>
        <div class="relative overflow-x-auto rounded-panel border border-border bg-panel">
          <table class="min-w-full divide-y divide-border">
            <caption class="sr-only">Fleet machines</caption>
            <thead>
              <tr :class="headRow">
                <th scope="col" class="px-4 py-3">Machine</th>
                <th scope="col" class="px-4 py-3">Kind</th>
                <th scope="col" class="px-4 py-3">State</th>
                <th scope="col" class="px-4 py-3">Slots used</th>
                <th scope="col" class="px-4 py-3">Instance</th>
                <th scope="col" class="px-4 py-3">Zone</th>
                <th scope="col" class="px-4 py-3">Launched</th>
                <th scope="col" class="px-4 py-3">Ended</th>
              </tr>
            </thead>
            <tbody class="divide-y divide-border text-sm text-ink">
              <tr v-if="overview.machines.length === 0">
                <td colspan="8" class="px-4 py-6 text-center text-ink-muted">No machine has run.</td>
              </tr>
              <tr v-for="m in overview.machines" :key="m.id" class="infra-machine-row align-top hover:bg-hover">
                <th scope="row" class="whitespace-nowrap px-4 py-3 text-left font-normal">
                  <RouterLink to="/admin/fleet" class="font-mono text-xs text-accent hover:text-ink-bright">{{ m.id }}</RouterLink>
                </th>
                <td class="whitespace-nowrap px-4 py-3">{{ machineKindLabel(m.kind) }}</td>
                <td class="whitespace-nowrap px-4 py-3" :class="machineStateClass(m.state)">{{ machineStateLabel(m.state) }}</td>
                <td class="whitespace-nowrap px-4 py-3">{{ m.slots_used }} of {{ m.slots }}</td>
                <td class="whitespace-nowrap px-4 py-3 font-mono text-xs">
                  {{ m.cloud_id || 'None yet' }}
                  <span v-if="m.instance_type" class="block text-ink-muted">{{ m.instance_type }}</span>
                  <span v-if="m.ec2_state" class="block font-sans" :class="ec2Mismatch(m) ? 'text-warn' : 'text-ink-muted'">EC2: {{ m.ec2_state }}</span>
                </td>
                <td class="whitespace-nowrap px-4 py-3 text-xs">{{ m.zone || '' }}</td>
                <td class="whitespace-nowrap px-4 py-3 text-xs text-ink-muted">
                  <RelativeTime v-if="m.launched_at" :value="m.launched_at" :now="now" />
                </td>
                <td class="px-4 py-3 text-xs text-ink-muted">
                  <template v-if="m.ended_at">
                    <RelativeTime :value="m.ended_at" :now="now" />
                    <span v-if="m.end_reason" class="block">{{ m.end_reason }}</span>
                  </template>
                </td>
              </tr>
            </tbody>
          </table>
        </div>
      </section>
    </template>
  </div>
</template>

<script setup lang="ts">
// /admin/infrastructure: the deployment as it runs (GET
// /api/admin/infrastructure). Above, at a glance: the programs steady, the
// tasks running, the load balancer's healthy targets, the release, the live
// lobby instances and the fleet's machines, and the warnings to look at first.
// Below: each program's ECS service with its tasks, stopped tasks and events,
// the target groups, the database and the cache (from AWS, which web reads at
// most every 20 seconds), then the lobby instances, the controller and the
// machines (from the database). On the local stack AWS is not read, and the
// page says so rather than showing anything made up. Polled every 30 seconds.
import { computed, ref } from 'vue';
import { RouterLink } from 'vue-router';
import RelativeTime from '../../components/RelativeTime.vue';
import StatTile from '../../components/charts/StatTile.vue';
import { fetchInfrastructure } from '../../api/infra';
import { usePolling } from '../../composables/usePolling';
import type { InfraCloudRead, InfraLobby, InfraMachine, InfraOverview, InfraService, InfraTask } from '../../types/infra';
import { machineKindLabel, machineStateLabel } from '../../utils/adminView';
import {
  countLabel,
  healthLabel,
  infraWarnings,
  listenerLabel,
  offRelease,
  primaryDeployment,
  rolloutLabel,
  serviceState,
  shortImage,
  targetLabel,
  targetTotals,
  taskSize,
  taskStatusLabel,
  taskTotals,
  toneBadgeClass,
  type Tone,
} from '../../utils/infraView';

const POLL_MS = 30_000;

const secondaryButton =
  'rounded-control border border-border bg-raised px-4 py-2 text-sm font-medium text-ink transition hover:bg-hover disabled:cursor-not-allowed disabled:opacity-40';
const headRow = 'bg-raised text-left text-xs font-semibold uppercase tracking-wider text-ink-muted';

const overview = ref<InfraOverview | null>(null);
const loadedAt = ref<number | null>(null);
const now = ref(Date.now());
const error = ref('');
const pollTrouble = ref(false);

// AWS's sections, when they were read.
const cloud = computed<InfraCloudRead | null>(() => (overview.value?.cloud.configured ? overview.value.cloud : null));
const warnings = computed(() => (overview.value ? infraWarnings(overview.value) : []));
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

// The deployments listed while there is more than one, or the one is not done.
function showDeployments(s: InfraService): boolean {
  return s.deployments.length > 1 || (s.deployments.length === 1 && s.deployments[0].rollout_state !== 'COMPLETED');
}

// "43" from "opennova-prod-web:43".
function revisionOf(taskDefinition: string): string {
  return taskDefinition.slice(taskDefinition.lastIndexOf(':') + 1);
}

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

function machineStateClass(state: string): string {
  if (state === 'failed' || state === 'lost') return 'text-danger';
  if (state === 'ready') return 'text-online';
  if (state === 'ended') return 'text-ink-muted';
  return 'text-warn';
}

// A live machine EC2 no longer runs, or an ended one it still does.
function ec2Mismatch(m: InfraMachine): boolean {
  const live = m.state !== 'ended' && m.state !== 'failed';
  const running = m.ec2_state === 'running' || m.ec2_state === 'pending';
  return live !== running;
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
    const result = await fetchInfrastructure(signal);
    if (signal.aborted) return false;
    now.value = Date.now();
    if (result.ok) {
      overview.value = result.value;
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
