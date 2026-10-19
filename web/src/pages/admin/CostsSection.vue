<template>
  <div class="space-y-8">
    <header class="flex flex-col gap-4 sm:flex-row sm:items-center sm:justify-between">
      <div>
        <h1 class="text-3xl font-semibold text-ink">AWS costs</h1>
        <p class="text-sm text-ink-muted">
          What this deployment's AWS account is charged this month: so far, the forecast for the month, by AWS service
          and against the monthly budget.
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
    <div v-if="costs === null && !error" class="animate-pulse rounded-panel border border-border bg-panel p-8 text-center text-ink-muted">
      Loading the costs…
    </div>

    <CostSummary v-if="costs" :costs="costs" :now="now" />
  </div>
</template>

<script setup lang="ts">
// /admin/costs: the deployment's AWS bill this month (GET /api/admin/costs,
// CostSummary): where it stands against the budget, the month so far, the
// forecast, the AWS services and the days. Web reads AWS at most every 6 hours,
// so the page reads once and on Refresh, never on a timer.
import { ref } from 'vue';
import CostSummary from '../../components/CostSummary.vue';
import RelativeTime from '../../components/RelativeTime.vue';
import { fetchCosts } from '../../api/costs';
import { usePolling } from '../../composables/usePolling';
import type { Costs } from '../../types/costs';

const secondaryButton =
  'rounded-control border border-border bg-raised px-4 py-2 text-sm font-medium text-ink transition hover:bg-hover disabled:cursor-not-allowed disabled:opacity-40';

const costs = ref<Costs | null>(null);
const error = ref('');
const loadedAt = ref<number | null>(null);
const now = ref(Date.now());

const poll = usePolling({
  async read(signal, background) {
    const result = await fetchCosts(signal);
    if (signal.aborted) return false;
    now.value = Date.now();
    if (result.ok) {
      costs.value = result.value;
      loadedAt.value = now.value;
      error.value = '';
      return true;
    }
    if (!background || costs.value === null) error.value = result.error.message;
    return false;
  },
  next: () => null,
});
</script>
