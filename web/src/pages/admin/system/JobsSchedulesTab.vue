<template>
  <section aria-label="Schedules">
    <div class="table-wrap">
      <table class="data-table">
        <caption class="sr-only">Periodic jobs</caption>
        <thead>
          <tr>
            <th scope="col">Kind</th>
            <th scope="col">How often</th>
            <th scope="col">Last run</th>
            <th scope="col">Next</th>
            <th scope="col"><span class="sr-only">Actions</span></th>
          </tr>
        </thead>
        <tbody>
          <TableState v-if="!overview || overview.schedules.length === 0" :colspan="5" :rows="overview?.schedules ?? null" empty="No schedules." />
          <tr v-for="s in overview?.schedules ?? []" :key="s.kind" class="jobs-schedule-row">
            <th scope="row" class="font-normal" :title="s.kind">{{ jobKindLabel(s.kind) }}</th>
            <td>
              {{ capitalized(everyLabel(s.interval_seconds)) }}
              <span v-if="s.conditional" class="block text-xs text-ink-muted">only with work</span>
              <span v-if="s.at_start" class="block text-xs text-ink-muted">and at each release's start</span>
            </td>
            <td>
              <template v-if="s.last">
                <RouterLink :to="`/admin/jobs/${s.last.id}`" :class="jobStateBadgeClass(s.last)">
                  {{ jobStateLabel(s.last) }}<span class="sr-only">: job {{ s.last.id }}</span>
                </RouterLink>
                <span class="ml-2 text-xs text-ink-muted">
                  <RelativeTime :value="s.last.finished_at ?? s.last.created_at ?? ''" :now="now" /><template v-if="s.last.manual">, by an admin</template>
                </span>
                <span v-if="s.last_finished && s.last_finished.state === 'failed'" class="block max-w-xs break-words text-xs text-danger">
                  {{ s.last_finished.last_error }}
                </span>
              </template>
              <span v-else class="text-ink-muted">Never</span>
            </td>
            <td class="whitespace-nowrap">{{ scheduleNextLabel(s, now) }}</td>
            <td class="whitespace-nowrap text-right">
              <button
                type="button"
                class="btn-ghost btn-sm"
                :disabled="busy || s.pending"
                :title="s.pending ? 'One is waiting or running' : undefined"
                @click="runNow(s)"
              >
                Run now<span class="sr-only">{{ ' ' + (s.kind) }}</span>
              </button>
            </td>
          </tr>
        </tbody>
      </table>
    </div>
  </section>
</template>

<script setup lang="ts">
// The jobs page's Schedules view: each periodic kind, how often it runs, its
// last run (to its job) and the next, and a run now.
import { ref } from 'vue';
import { RouterLink } from 'vue-router';
import RelativeTime from '../../../components/RelativeTime.vue';
import TableState from '../../../components/ui/TableState.vue';
import { runJobNow } from '../../../api/jobs';
import { confirmAction } from '../../../composables/useConfirm';
import type { JobSchedule, JobsOverview } from '../../../types/jobs';
import { everyLabel, jobKindLabel, jobStateBadgeClass, jobStateLabel, scheduleNextLabel } from '../../../utils/jobsView';

defineProps<{
  overview: JobsOverview | null;
  now: number;
}>();

const emit = defineEmits<{
  notice: [notice: { ok: boolean; text: string }];
  changed: [];
}>();

const busy = ref(false);

function capitalized(text: string): string {
  return text.charAt(0).toUpperCase() + text.slice(1);
}

async function runNow(s: JobSchedule) {
  if (busy.value) return;
  if (!await confirmAction(`Run "${jobKindLabel(s.kind)}" now? A job is queued beside its schedule.`)) return;
  busy.value = true;
  const result = await runJobNow(s.kind);
  busy.value = false;
  emit('notice', result.ok ? { ok: true, text: `"${jobKindLabel(s.kind)}" is queued (job ${result.value.id}).` } : { ok: false, text: result.error.message });
  emit('changed');
}
</script>
