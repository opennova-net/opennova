<template>
  <div class="space-y-10">
    <header class="flex flex-col gap-4 sm:flex-row sm:items-center sm:justify-between">
      <div>
        <h1 class="text-3xl font-semibold text-ink">Background jobs</h1>
        <p class="text-sm text-ink-muted">
          The work NovaWorld does off its requests: the fleet's cloud calls, the billing sync with Stripe, the mail and the
          prunes. Workers take each job from one queue in the database.
        </p>
      </div>
      <div class="flex items-center gap-3">
        <p v-if="loadedAt" class="text-xs text-ink-muted">Updated <RelativeTime :value="loadedAt" :now="now" /></p>
        <button type="button" :class="secondaryButton" :disabled="poll.loading.value" @click="poll.refresh">
          {{ poll.loading.value ? 'Refreshing…' : 'Refresh' }}
        </button>
      </div>
    </header>

    <!-- Live regions, always in the page so a screen reader hears what lands in
         them; the boxes below are their visible copies. -->
    <div role="status" class="sr-only">{{ message }}</div>
    <div role="alert" class="sr-only">{{ error }}</div>
    <div v-if="message" aria-hidden="true" class="rounded-panel border border-online/40 bg-online/10 p-4 text-sm text-online">{{ message }}</div>
    <div v-if="error" aria-hidden="true" class="rounded-panel border border-danger/40 bg-danger/10 p-4 text-sm text-danger">{{ error }}</div>
    <p v-if="pollTrouble" class="text-xs text-warn">The last refresh did not get through; this page keeps trying.</p>

    <ul v-if="warnings.length" class="jobs-warnings space-y-2">
      <li v-for="w in warnings" :key="w" class="rounded-panel border border-warn/40 bg-warn/10 p-4 text-sm text-warn">{{ w }}</li>
    </ul>

    <dl v-if="overview" class="grid grid-cols-1 gap-3 min-[420px]:grid-cols-2 lg:grid-cols-3">
      <StatTile label="Waiting now" :value="String(overview.totals.due)" :detail="laterDetail" />
      <StatTile
        label="Queue latency"
        :value="overview.totals.oldest_wait_seconds === null ? 'None waiting' : secondsLabel(overview.totals.oldest_wait_seconds)"
        detail="How long the oldest due job has waited for a worker"
      />
      <StatTile
        label="Running"
        :value="String(overview.totals.running)"
        :detail="`${overview.workers.length} worker thread${overview.workers.length === 1 ? '' : 's'} seen in the last hour`"
      />
      <StatTile label="Finished in the last hour" :value="formatCount(finishedHour)" :detail="rateDetail">
        <template #trend>
          <Sparkline :values="perMinute" label="Jobs finished a minute, last hour" :format="formatCount" />
        </template>
      </StatTile>
      <StatTile
        label="Failed in the last day"
        :value="formatCount(overview.totals.failed_day)"
        :detail="`${formatCount(overview.totals.failed)} failed in all, kept until the prune`"
      />
      <StatTile
        label="Mean run time"
        :value="overview.totals.mean_run_seconds === null ? 'None ran' : secondsLabel(overview.totals.mean_run_seconds)"
        detail="Of the jobs done in the last hour, from a worker's claim to the end"
      />
    </dl>

    <!-- The failed jobs to look at. -->
    <section class="space-y-4" aria-labelledby="jobs-attention">
      <h2 id="jobs-attention" class="text-xl font-semibold text-ink">Needs attention</h2>
      <p class="text-sm text-ink-muted">
        Jobs that failed in the last day, their attempts used up or failed for good (an admin's cancels left out). A retry
        queues one again to run now with one more attempt.
      </p>
      <div class="relative overflow-x-auto rounded-panel border border-border bg-panel">
        <table class="min-w-full divide-y divide-border">
          <caption class="sr-only">Failed jobs</caption>
          <thead>
            <tr :class="headRow">
              <th scope="col" class="px-4 py-3">Job</th>
              <th scope="col" class="px-4 py-3">Attempts</th>
              <th scope="col" class="px-4 py-3">Why</th>
              <th scope="col" class="px-4 py-3">Failed</th>
              <th scope="col" class="px-4 py-3 text-right">Actions</th>
            </tr>
          </thead>
          <tbody class="divide-y divide-border text-sm text-ink">
            <tr v-if="overview === null">
              <td colspan="5" class="px-4 py-6 text-center text-ink-muted">{{ poll.loading.value ? 'Loading…' : 'Not loaded.' }}</td>
            </tr>
            <tr v-else-if="overview.attention.length === 0">
              <td colspan="5" class="px-4 py-6 text-center text-ink-muted">No job failed in the last day.</td>
            </tr>
            <tr v-for="j in overview?.attention ?? []" :key="j.id" class="jobs-attention-row align-top hover:bg-hover">
              <th scope="row" class="px-4 py-3 text-left font-normal">
                <RouterLink :to="`/admin/jobs/${j.id}`" class="text-accent hover:text-ink-bright">{{ jobKindLabel(j.kind) }}</RouterLink>
                <span class="block font-mono text-xs text-ink-muted">{{ j.kind }}, job {{ j.id }}</span>
              </th>
              <td class="whitespace-nowrap px-4 py-3">{{ attemptsLabel(j) }}</td>
              <td class="max-w-md break-words px-4 py-3 text-danger">{{ j.last_error || 'No reason recorded' }}</td>
              <td class="whitespace-nowrap px-4 py-3 text-xs text-ink-muted">
                <RelativeTime v-if="j.finished_at" :value="j.finished_at" :now="now" />
              </td>
              <td class="whitespace-nowrap px-4 py-3 text-right">
                <button
                  v-if="j.retryable"
                  type="button"
                  :disabled="busy"
                  class="text-accent hover:text-ink-bright disabled:opacity-40"
                  @click="retry(j)"
                >
                  Retry now<span class="sr-only"> job {{ j.id }}</span>
                </button>
                <span v-else class="text-xs text-ink-muted" title="Its secret was dropped when it ended">Cannot run again</span>
              </td>
            </tr>
          </tbody>
        </table>
      </div>
    </section>

    <!-- Each kind's counts. -->
    <section class="space-y-4" aria-labelledby="jobs-kinds">
      <h2 id="jobs-kinds" class="text-xl font-semibold text-ink">By kind</h2>
      <div class="relative overflow-x-auto rounded-panel border border-border bg-panel">
        <table class="min-w-full divide-y divide-border">
          <caption class="sr-only">Jobs by kind and state</caption>
          <thead>
            <tr :class="headRow">
              <th scope="col" class="px-4 py-3">Kind</th>
              <th scope="col" class="px-4 py-3">Waiting</th>
              <th scope="col" class="px-4 py-3">Running</th>
              <th scope="col" class="px-4 py-3">Done</th>
              <th scope="col" class="px-4 py-3">Failed</th>
              <th scope="col" class="px-4 py-3">Oldest wait</th>
              <th scope="col" class="px-4 py-3">Mean run time</th>
            </tr>
          </thead>
          <tbody class="divide-y divide-border text-sm text-ink">
            <tr v-if="overview !== null && overview.kinds.length === 0">
              <td colspan="7" class="px-4 py-6 text-center text-ink-muted">No job in the queue's history.</td>
            </tr>
            <tr v-for="k in overview?.kinds ?? []" :key="k.kind" class="jobs-kind-row align-top hover:bg-hover">
              <th scope="row" class="px-4 py-3 text-left font-normal">
                {{ jobKindLabel(k.kind) }}
                <span class="block font-mono text-xs text-ink-muted">{{ k.kind }}</span>
              </th>
              <td class="whitespace-nowrap px-4 py-3">
                {{ k.due }} due<span v-if="k.queued > k.due" class="block text-xs text-ink-muted">{{ k.queued - k.due }} later</span>
              </td>
              <td class="whitespace-nowrap px-4 py-3">{{ k.running }}</td>
              <td class="whitespace-nowrap px-4 py-3">
                {{ formatCount(k.done_hour) }} in the last hour
                <span class="block text-xs text-ink-muted">{{ formatCount(k.done_day) }} in the last day</span>
              </td>
              <td class="whitespace-nowrap px-4 py-3" :class="k.failed_day > 0 ? 'text-danger' : ''">
                {{ formatCount(k.failed_day) }} in the last day
                <span class="block text-xs text-ink-muted">{{ formatCount(k.failed) }} kept</span>
              </td>
              <td class="whitespace-nowrap px-4 py-3">{{ k.oldest_wait_seconds === null ? 'None waiting' : secondsLabel(k.oldest_wait_seconds) }}</td>
              <td class="whitespace-nowrap px-4 py-3">{{ k.mean_run_seconds === null ? 'None ran' : secondsLabel(k.mean_run_seconds) }}</td>
            </tr>
          </tbody>
        </table>
      </div>
    </section>

    <!-- The workers: the jobs they hold, and the threads seen. -->
    <section class="space-y-4" aria-labelledby="jobs-workers">
      <h2 id="jobs-workers" class="text-xl font-semibold text-ink">Workers</h2>
      <p class="text-sm text-ink-muted">
        A worker holds its job under a lease of a minute that it renews while the job runs. A lease that ran out means its
        worker went away; the keeper queues the job again. Worker threads show once they have taken a job in the last
        hour.
      </p>
      <h3 class="text-sm font-semibold text-ink">Running now</h3>
      <div class="relative overflow-x-auto rounded-panel border border-border bg-panel">
        <table class="min-w-full divide-y divide-border">
          <caption class="sr-only">Running jobs and their workers</caption>
          <thead>
            <tr :class="headRow">
              <th scope="col" class="px-4 py-3">Job</th>
              <th scope="col" class="px-4 py-3">Worker</th>
              <th scope="col" class="px-4 py-3">Running for</th>
              <th scope="col" class="px-4 py-3">Lease</th>
              <th scope="col" class="px-4 py-3">Attempt</th>
            </tr>
          </thead>
          <tbody class="divide-y divide-border text-sm text-ink">
            <tr v-if="overview !== null && overview.running.length === 0">
              <td colspan="5" class="px-4 py-6 text-center text-ink-muted">No job is running.</td>
            </tr>
            <tr v-for="j in overview?.running ?? []" :key="j.id" class="jobs-running-row align-top hover:bg-hover">
              <th scope="row" class="px-4 py-3 text-left font-normal">
                <RouterLink :to="`/admin/jobs/${j.id}`" class="text-accent hover:text-ink-bright">{{ jobKindLabel(j.kind) }}</RouterLink>
                <span class="block font-mono text-xs text-ink-muted">{{ j.kind }}, job {{ j.id }}</span>
              </th>
              <td class="whitespace-nowrap px-4 py-3 font-mono text-xs">{{ j.worker_id ?? 'Unknown' }}</td>
              <td class="whitespace-nowrap px-4 py-3">{{ j.running_seconds === null ? 'Unknown' : secondsLabel(j.running_seconds) }}</td>
              <td class="whitespace-nowrap px-4 py-3" :class="leaseExpired(j) ? 'font-semibold text-danger' : 'text-ink-muted'">
                {{ leaseLabel(j) || 'Unknown' }}
              </td>
              <td class="whitespace-nowrap px-4 py-3">{{ attemptsLabel(j) }}</td>
            </tr>
          </tbody>
        </table>
      </div>
      <h3 class="text-sm font-semibold text-ink">Worker threads</h3>
      <div class="relative overflow-x-auto rounded-panel border border-border bg-panel">
        <table class="min-w-full divide-y divide-border">
          <caption class="sr-only">Worker threads seen in the last hour</caption>
          <thead>
            <tr :class="headRow">
              <th scope="col" class="px-4 py-3">Worker process</th>
              <th scope="col" class="px-4 py-3">Thread</th>
              <th scope="col" class="px-4 py-3">Holding</th>
              <th scope="col" class="px-4 py-3">Last hour</th>
              <th scope="col" class="px-4 py-3">Last active</th>
            </tr>
          </thead>
          <tbody class="divide-y divide-border text-sm text-ink">
            <tr v-if="overview !== null && overview.workers.length === 0">
              <td colspan="5" class="px-4 py-6 text-center text-ink-muted">No worker has taken a job in the last hour.</td>
            </tr>
            <tr v-for="w in workers" :key="w.worker_id" class="jobs-worker-row align-top hover:bg-hover">
              <td class="whitespace-nowrap px-4 py-3 font-mono text-xs">{{ w.process }}</td>
              <th scope="row" class="whitespace-nowrap px-4 py-3 text-left font-mono text-xs font-normal">{{ w.worker_id }}</th>
              <td class="whitespace-nowrap px-4 py-3">
                <RouterLink v-if="w.job_id !== null" :to="`/admin/jobs/${w.job_id}`" class="text-accent hover:text-ink-bright">
                  {{ jobKindLabel(w.job_kind ?? '') }}, job {{ w.job_id }}
                </RouterLink>
                <span v-else class="text-ink-muted">Idle</span>
              </td>
              <td class="whitespace-nowrap px-4 py-3">
                {{ w.done }} done<span v-if="w.failed" class="text-danger">, {{ w.failed }} failed</span>
              </td>
              <td class="whitespace-nowrap px-4 py-3 text-xs text-ink-muted">
                <RelativeTime v-if="w.last_active_at" :value="w.last_active_at" :now="now" />
              </td>
            </tr>
          </tbody>
        </table>
      </div>
    </section>

    <!-- The schedules. -->
    <section class="space-y-4" aria-labelledby="jobs-schedules">
      <h2 id="jobs-schedules" class="text-xl font-semibold text-ink">Schedules</h2>
      <p v-if="overview" class="jobs-controller text-sm text-ink-muted">
        <template v-if="overview.controller.leader">
          The controller <span class="font-mono">{{ overview.controller.leader }}</span> enqueues them<template
            v-if="overview.controller.elected_at"
          >, leading since <RelativeTime :value="overview.controller.elected_at" :now="now" /></template>.
        </template>
        <template v-else>No controller has led yet.</template>
        <template v-if="overview.controller.last_scheduled_at">
          The last scheduled job came in <RelativeTime :value="overview.controller.last_scheduled_at" :now="now" />.
        </template>
      </p>
      <div class="relative overflow-x-auto rounded-panel border border-border bg-panel">
        <table class="min-w-full divide-y divide-border">
          <caption class="sr-only">Periodic jobs</caption>
          <thead>
            <tr :class="headRow">
              <th scope="col" class="px-4 py-3">Kind</th>
              <th scope="col" class="px-4 py-3">How often</th>
              <th scope="col" class="px-4 py-3">Last run</th>
              <th scope="col" class="px-4 py-3">Next</th>
              <th scope="col" class="px-4 py-3 text-right">Actions</th>
            </tr>
          </thead>
          <tbody class="divide-y divide-border text-sm text-ink">
            <tr v-if="overview !== null && overview.schedules.length === 0">
              <td colspan="5" class="px-4 py-6 text-center text-ink-muted">This service schedules no job.</td>
            </tr>
            <tr v-for="s in overview?.schedules ?? []" :key="s.kind" class="jobs-schedule-row align-top hover:bg-hover">
              <th scope="row" class="px-4 py-3 text-left font-normal">
                {{ jobKindLabel(s.kind) }}
                <span class="block font-mono text-xs text-ink-muted">{{ s.kind }}</span>
              </th>
              <td class="px-4 py-3">
                {{ capitalized(everyLabel(s.interval_seconds)) }}
                <span v-if="s.conditional" class="block text-xs text-ink-muted">only while there is work for it</span>
                <span v-if="s.at_start" class="block text-xs text-ink-muted">and when a controller starts</span>
              </td>
              <td class="px-4 py-3">
                <template v-if="s.last">
                  <RouterLink :to="`/admin/jobs/${s.last.id}`" :class="jobStateBadgeClass(s.last)">{{ jobStateLabel(s.last) }}</RouterLink>
                  <span class="ml-2 text-xs text-ink-muted">
                    <RelativeTime :value="s.last.finished_at ?? s.last.created_at ?? ''" :now="now" /><template v-if="s.last.manual">, run by an admin</template>
                  </span>
                  <span
                    v-if="s.last_finished && s.last_finished.state === 'failed'"
                    class="block max-w-xs break-words text-xs text-danger"
                  >
                    {{ s.last_finished.last_error }}
                  </span>
                </template>
                <span v-else class="text-ink-muted">Never</span>
              </td>
              <td class="whitespace-nowrap px-4 py-3">{{ scheduleNextLabel(s, now) }}</td>
              <td class="whitespace-nowrap px-4 py-3 text-right">
                <button
                  type="button"
                  :disabled="busy || s.pending"
                  :title="s.pending ? 'One of this kind is waiting or running' : undefined"
                  class="text-accent hover:text-ink-bright disabled:opacity-40"
                  @click="runNow(s)"
                >
                  Run now<span class="sr-only"> {{ s.kind }}</span>
                </button>
              </td>
            </tr>
          </tbody>
        </table>
      </div>
    </section>

    <!-- Every job, filtered and paged. -->
    <section class="space-y-4" aria-labelledby="jobs-list">
      <h2 id="jobs-list" class="text-xl font-semibold text-ink">Jobs</h2>
      <form class="jobs-filter flex flex-wrap items-end gap-3" @submit.prevent="applyFilter">
        <label class="space-y-1">
          <span :class="labelClass">State</span>
          <select v-model="filter.state" name="state" :class="fieldClass">
            <option value="">Any</option>
            <option value="queued">Waiting</option>
            <option value="running">Running</option>
            <option value="done">Done</option>
            <option value="failed">Failed or cancelled</option>
          </select>
        </label>
        <label class="space-y-1">
          <span :class="labelClass">Kind</span>
          <select v-model="filter.kind" name="kind" :class="fieldClass">
            <option value="">Any</option>
            <option v-for="k in kindOptions" :key="k" :value="k">{{ jobKindLabel(k) }}</option>
          </select>
        </label>
        <label class="space-y-1">
          <span :class="labelClass">Created from (UTC)</span>
          <input v-model="filter.from" name="from" type="datetime-local" :class="fieldClass" />
        </label>
        <label class="space-y-1">
          <span :class="labelClass">Created before (UTC)</span>
          <input v-model="filter.to" name="to" type="datetime-local" :class="fieldClass" />
        </label>
        <label class="space-y-1">
          <span :class="labelClass">Key contains</span>
          <input
            v-model="filter.q"
            name="q"
            type="search"
            autocomplete="off"
            maxlength="128"
            placeholder="A dedupe or schedule key"
            :class="fieldClass"
          />
        </label>
        <button type="submit" :class="secondaryButton">Apply</button>
        <button type="button" :class="secondaryButton" @click="resetFilter">Reset</button>
      </form>
      <p v-if="filterError" class="text-sm text-danger">{{ filterError }}</p>
      <div class="relative overflow-x-auto rounded-panel border border-border bg-panel">
        <table class="min-w-full divide-y divide-border">
          <caption class="sr-only">Jobs, newest first</caption>
          <thead>
            <tr :class="headRow">
              <th scope="col" class="px-4 py-3">Job</th>
              <th scope="col" class="px-4 py-3">State</th>
              <th scope="col" class="px-4 py-3">Attempts</th>
              <th scope="col" class="px-4 py-3">Created</th>
              <th scope="col" class="px-4 py-3">Ended</th>
              <th scope="col" class="px-4 py-3">Key</th>
              <th scope="col" class="px-4 py-3 text-right">Actions</th>
            </tr>
          </thead>
          <tbody class="divide-y divide-border text-sm text-ink">
            <tr v-if="page === null">
              <td colspan="7" class="px-4 py-6 text-center text-ink-muted">{{ poll.loading.value ? 'Loading…' : 'Not loaded.' }}</td>
            </tr>
            <tr v-else-if="page.jobs.length === 0">
              <td colspan="7" class="px-4 py-6 text-center text-ink-muted">No job matches.</td>
            </tr>
            <tr v-for="j in page?.jobs ?? []" :key="j.id" class="jobs-list-row align-top hover:bg-hover">
              <th scope="row" class="px-4 py-3 text-left font-normal">
                <RouterLink :to="`/admin/jobs/${j.id}`" class="text-accent hover:text-ink-bright">{{ jobKindLabel(j.kind) }}</RouterLink>
                <span class="block font-mono text-xs text-ink-muted">{{ j.kind }}, job {{ j.id }}</span>
              </th>
              <td class="whitespace-nowrap px-4 py-3">
                <span :class="jobStateBadgeClass(j)">{{ jobStateLabel(j) }}</span>
                <span v-if="j.state === 'failed' && j.last_error" class="block max-w-xs whitespace-normal break-words text-xs text-danger">{{ j.last_error }}</span>
              </td>
              <td class="whitespace-nowrap px-4 py-3">{{ attemptsLabel(j) }}</td>
              <td class="whitespace-nowrap px-4 py-3 text-xs text-ink-muted">
                <RelativeTime v-if="j.created_at" :value="j.created_at" :now="now" />
              </td>
              <td class="whitespace-nowrap px-4 py-3 text-xs text-ink-muted">
                <RelativeTime v-if="j.finished_at" :value="j.finished_at" :now="now" />
                <template v-else-if="j.state === 'queued' && j.run_at">due <RelativeTime :value="j.run_at" :now="now" /></template>
              </td>
              <td class="max-w-xs break-all px-4 py-3 font-mono text-xs text-ink-muted">{{ j.schedule_key ?? j.dedupe_key ?? '' }}</td>
              <td class="space-x-3 whitespace-nowrap px-4 py-3 text-right">
                <button v-if="j.retryable" type="button" :disabled="busy" class="text-accent hover:text-ink-bright disabled:opacity-40" @click="retry(j)">
                  Retry now<span class="sr-only"> job {{ j.id }}</span>
                </button>
                <button v-if="j.cancellable" type="button" :disabled="busy" class="text-danger/80 hover:text-danger disabled:opacity-40" @click="cancel(j)">
                  Cancel<span class="sr-only"> job {{ j.id }}</span>
                </button>
              </td>
            </tr>
          </tbody>
        </table>
      </div>
      <nav v-if="page" aria-label="Job pages" class="flex flex-wrap items-center justify-between gap-3 text-sm text-ink-muted">
        <span class="jobs-page-summary">{{ pageSummary }}</span>
        <span class="flex gap-2">
          <button type="button" :class="secondaryButton" :disabled="cursors.length === 0" @click="newerPage">Newer</button>
          <button type="button" :class="secondaryButton" :disabled="page.next_before === null" @click="olderPage">Older</button>
        </span>
      </nav>
    </section>

    <!-- The prune. -->
    <section class="space-y-4 rounded-panel border border-border p-4 sm:p-6" aria-labelledby="jobs-prune">
      <h2 id="jobs-prune" class="text-xl font-semibold text-ink">Prune finished jobs</h2>
      <p class="text-sm text-ink-muted">
        Finished jobs are kept 14 days, then pruned every hour. Prune sooner to delete the done and failed jobs older than
        the time chosen; a worker does it within a few seconds. Waiting and running jobs are never pruned.
      </p>
      <form class="flex flex-wrap items-end gap-3" @submit.prevent="prune">
        <label class="space-y-1">
          <span :class="labelClass">Keep the last</span>
          <select v-model.number="keepDays" name="keep_days" :class="fieldClass">
            <option v-for="d in KEEP_DAYS" :key="d" :value="d">{{ d }} day{{ d === 1 ? '' : 's' }}</option>
          </select>
        </label>
        <button type="submit" :disabled="busy" class="rounded-control border border-danger px-4 py-2 text-sm font-medium text-danger transition hover:bg-danger/10 disabled:opacity-40">
          Prune now…
        </button>
      </form>
    </section>
  </div>
</template>

<script setup lang="ts">
// /admin/jobs: the background jobs (GET /api/admin/jobs/overview): at a
// glance, how many wait and how long the oldest has, how many run, the last
// hour's jobs a minute, the failures; the failed jobs of the last day, each
// with a retry; each kind's counts; the running jobs with their workers and
// leases (a lease run out is a worker gone) and the worker threads seen; the
// schedules (how often, the last run and its result, the next, run now); a
// page of jobs (GET /api/admin/jobs) by state, kind, creation time and key,
// newest first, each to its own page (/admin/jobs/<id>), with a retry or a
// cancel; and the prune (POST /api/admin/jobs/prune). Every action asks first,
// then reads again. Polled every 15 s, every 5 s while a job waits or runs.
import { computed, reactive, ref } from 'vue';
import { RouterLink } from 'vue-router';
import RelativeTime from '../../components/RelativeTime.vue';
import Sparkline from '../../components/charts/Sparkline.vue';
import StatTile from '../../components/charts/StatTile.vue';
import { cancelJob, fetchJobs, fetchJobsOverview, pruneJobs, retryJob, runJobNow } from '../../api/jobs';
import { usePolling } from '../../composables/usePolling';
import type { Job, JobPage, JobQuery, JobSchedule, JobState, JobsOverview } from '../../types/jobs';
import { formatCount, formatDuration, timeMs } from '../../utils/format';
import {
  attemptsLabel,
  everyLabel,
  finishedPerMinute,
  jobKindLabel,
  jobStateBadgeClass,
  jobStateLabel,
  leaseExpired,
  leaseLabel,
  scheduleNextLabel,
  secondsLabel,
  utcInputSeconds,
} from '../../utils/jobsView';

const BUSY_POLL_MS = 5_000;
const IDLE_POLL_MS = 15_000;
const PAGE_SIZE = 50;
const KEEP_DAYS = [1, 3, 7, 14] as const;
// The oldest due job's wait past which the page warns.
const LATENCY_WARN_S = 60;

const headRow = 'bg-raised text-left text-xs font-semibold uppercase tracking-wider text-ink-muted';
const labelClass = 'block text-xs font-medium text-ink-muted';
const fieldClass = 'rounded-control border border-border bg-surface px-3 py-2 text-sm text-ink focus:border-accent focus:outline-none';
const secondaryButton =
  'rounded-control border border-border bg-raised px-4 py-2 text-sm font-medium text-ink transition hover:bg-hover disabled:cursor-not-allowed disabled:opacity-40';

const overview = ref<JobsOverview | null>(null);
const page = ref<JobPage | null>(null);
const busy = ref(false);
const loadedAt = ref<number | null>(null);
const now = ref(Date.now());
const message = ref('');
const error = ref('');
const pollTrouble = ref(false);
const filterError = ref('');
const keepDays = ref<number>(14);
// The form's fields, and the query the list reads (set by Apply).
const filter = reactive({ state: '' as '' | JobState, kind: '', from: '', to: '', q: '' });
const query = ref<JobQuery>({});
// The `before` of each page before this one (the newest first is none).
const cursors = ref<(number | undefined)[]>([]);
// The error banner shows a failed read (not an action's refusal).
let errorFromLoad = false;

const finishedHour = computed(() => (overview.value ? overview.value.totals.done_hour + overview.value.totals.failed_hour : 0));
const perMinute = computed(() => finishedPerMinute(overview.value?.per_minute ?? []));

const laterDetail = computed(() => {
  const t = overview.value?.totals;
  if (!t) return undefined;
  const later = t.queued - t.due;
  return later > 0 ? `Due and waiting for a worker; ${formatCount(later)} more to run later` : 'Due and waiting for a worker';
});

const rateDetail = computed(() => {
  const t = overview.value?.totals;
  if (!t) return undefined;
  const rate = finishedHour.value / 60;
  const shown = rate === 0 ? '0' : rate < 10 ? rate.toFixed(1) : String(Math.round(rate));
  return `${shown} a minute on average; ${formatCount(t.done_day)} done in the last day`;
});

// The worker threads by process, then thread.
const workers = computed(() =>
  [...(overview.value?.workers ?? [])].sort((a, b) => a.process.localeCompare(b.process) || a.worker_id.localeCompare(b.worker_id)),
);

// The kinds the filter offers: every kind the queue holds and every schedule's.
const kindOptions = computed(() => {
  const kinds = new Set<string>();
  for (const k of overview.value?.kinds ?? []) kinds.add(k.kind);
  for (const s of overview.value?.schedules ?? []) kinds.add(s.kind);
  return [...kinds].sort();
});

// What an operator should look at first.
const warnings = computed(() => {
  const o = overview.value;
  if (!o) return [];
  const out: string[] = [];
  const lapsed = o.running.filter(leaseExpired).length;
  if (lapsed > 0) {
    out.push(
      `${lapsed} running job${lapsed === 1 ? "'s" : "s'"} lease ran out: ${lapsed === 1 ? 'its worker' : 'their workers'} went away. The keeper queues ${lapsed === 1 ? 'it' : 'them'} again within seconds; a job stuck here means no worker runs.`,
    );
  }
  const wait = o.totals.oldest_wait_seconds;
  if (wait !== null && wait >= LATENCY_WARN_S) {
    out.push(`The oldest due job has waited ${secondsLabel(wait)} for a worker: the workers are busy, or none runs.`);
  }
  // A controller schedules every periodic kind; none enqueued for three of the
  // shortest intervals (two minutes at least) is one not leading.
  if (o.schedules.length > 0) {
    const shortest = Math.min(...o.schedules.map((s) => s.interval_seconds));
    const allowed = Math.max(3 * shortest, 120) * 1000;
    const last = o.controller.last_scheduled_at ? timeMs(o.controller.last_scheduled_at) : Number.NaN;
    const at = timeMs(o.at);
    if (Number.isNaN(last)) {
      out.push('No schedule has enqueued a job yet. Is a controller leading?');
    } else if (at - last > allowed) {
      out.push(`No schedule has enqueued a job for ${formatDuration(at - last)}. Is a controller leading?`);
    }
  }
  return out;
});

const pageSummary = computed(() => {
  const p = page.value;
  if (!p) return '';
  if (p.jobs.length === 0) return `${formatCount(p.total)} jobs`;
  const first = cursors.value.length * PAGE_SIZE + 1;
  const last = first + p.jobs.length - 1;
  return `Jobs ${formatCount(first)} to ${formatCount(last)} of ${formatCount(p.total)}, newest first`;
});

function capitalized(text: string): string {
  return text.charAt(0).toUpperCase() + text.slice(1);
}

function flash(text: string) {
  message.value = text;
  error.value = '';
}

function fail(text: string) {
  error.value = text;
  message.value = '';
  errorFromLoad = false;
}

const poll = usePolling({
  async read(signal, background) {
    const [o, p] = await Promise.all([fetchJobsOverview(signal), fetchJobs({ ...query.value, limit: PAGE_SIZE }, signal)]);
    if (signal.aborted) return false;
    now.value = Date.now();
    if (o.ok) overview.value = o.value;
    if (p.ok) page.value = p.value;
    if (o.ok && p.ok) {
      loadedAt.value = now.value;
      pollTrouble.value = false;
      if (errorFromLoad) {
        error.value = '';
        errorFromLoad = false;
      }
      return true;
    }
    const failure = !o.ok ? o.error : !p.ok ? p.error : null;
    if (background && overview.value !== null) {
      pollTrouble.value = true;
    } else if (failure) {
      fail(failure.message);
      errorFromLoad = true;
    }
    return false;
  },
  next: () => {
    const t = overview.value?.totals;
    return t && (t.running > 0 || t.due > 0) ? BUSY_POLL_MS : IDLE_POLL_MS;
  },
});

function applyFilter() {
  const from = utcInputSeconds(filter.from);
  const to = utcInputSeconds(filter.to);
  if ((filter.from && from === null) || (filter.to && to === null)) {
    filterError.value = 'A date the filter could not read: pick it again.';
    return;
  }
  if (from !== null && to !== null && to <= from) {
    filterError.value = 'The end of the range comes before its start.';
    return;
  }
  filterError.value = '';
  const q = filter.q.trim();
  query.value = {
    ...(filter.state ? { state: filter.state } : {}),
    ...(filter.kind ? { kind: filter.kind } : {}),
    ...(from !== null ? { from } : {}),
    ...(to !== null ? { to } : {}),
    ...(q ? { q } : {}),
  };
  cursors.value = [];
  void poll.refresh();
}

function resetFilter() {
  Object.assign(filter, { state: '', kind: '', from: '', to: '', q: '' });
  applyFilter();
}

function olderPage() {
  const next = page.value?.next_before;
  if (next === null || next === undefined) return;
  cursors.value = [...cursors.value, query.value.before];
  query.value = { ...query.value, before: next };
  void poll.refresh();
}

function newerPage() {
  if (cursors.value.length === 0) return;
  const before = cursors.value[cursors.value.length - 1];
  cursors.value = cursors.value.slice(0, -1);
  const rest: JobQuery = { ...query.value };
  delete rest.before;
  query.value = before === undefined ? rest : { ...rest, before };
  void poll.refresh();
}

async function retry(j: Job) {
  if (busy.value) return;
  const next = j.attempts + 1;
  if (!window.confirm(`Retry "${jobKindLabel(j.kind)}" (job ${j.id}) now? It gets one more attempt, ${next} of ${next}.`)) return;
  busy.value = true;
  const result = await retryJob(j.id);
  busy.value = false;
  if (!result.ok) fail(result.error.message);
  else flash(`Job ${j.id} is queued again to run now.`);
  await poll.readAgain();
}

async function cancel(j: Job) {
  if (busy.value) return;
  if (!window.confirm(`Cancel "${jobKindLabel(j.kind)}" (job ${j.id})? It will not run, and it is marked failed as cancelled by you.`)) return;
  busy.value = true;
  const result = await cancelJob(j.id);
  busy.value = false;
  if (!result.ok) fail(result.error.message);
  else flash(`Job ${j.id} is cancelled.`);
  await poll.readAgain();
}

async function runNow(s: JobSchedule) {
  if (busy.value) return;
  if (!window.confirm(`Run "${jobKindLabel(s.kind)}" now? A job of this kind is queued at once, beside its schedule; a worker takes it within seconds.`)) return;
  busy.value = true;
  const result = await runJobNow(s.kind);
  busy.value = false;
  if (!result.ok) fail(result.error.message);
  else flash(`"${jobKindLabel(s.kind)}" is queued to run now (job ${result.value.id}).`);
  await poll.readAgain();
}

async function prune() {
  if (busy.value) return;
  const days = keepDays.value;
  if (!window.confirm(`Delete every job that finished more than ${days} day${days === 1 ? '' : 's'} ago? Their history goes for good.`)) return;
  busy.value = true;
  const result = await pruneJobs(days);
  busy.value = false;
  if (!result.ok) fail(result.error.message);
  else flash(`The finished jobs older than ${days} day${days === 1 ? '' : 's'} are being pruned (job ${result.value.id}).`);
  await poll.readAgain();
}
</script>
