<template>
  <div class="space-y-8">
    <AdminPageHeader
      title="AWS costs"
      :description="description"
      :loaded-at="loadedAt"
      :now="now"
      :loading="poll.loading.value"
      @refresh="poll.refresh"
    />

    <!-- A live region, always in the page, the box itself while it holds something. -->
    <div role="alert" :class="error ? 'rounded-panel border border-danger/40 bg-danger/10 p-4 text-sm text-danger' : 'sr-only'">{{ error }}</div>
    <div v-if="costs === null && !error" class="animate-pulse rounded-panel border border-border bg-panel p-8 text-center text-ink-muted">
      Loading the costs…
    </div>

    <CostSummary v-if="costs" :costs="costs" :now="now" />
  </div>
</template>

<script setup lang="ts">
// /admin/costs: the configured AWS accounts' monthly bill, with combined
// totals and charts and account budgets. Web caches successful reads for
// 6 hours, so the page reads once and on Refresh, never on a timer.
import AdminPageHeader from '../../components/AdminPageHeader.vue';
import { computed, ref } from 'vue';
import CostSummary from '../../components/CostSummary.vue';
import { fetchCosts } from '../../api/costs';
import { usePolling } from '../../composables/usePolling';
import type { Costs } from '../../types/costs';


const costs = ref<Costs | null>(null);
const error = ref('');
const loadedAt = ref<number | null>(null);
const now = ref(Date.now());
const description = computed(() => {
  const accounts = costs.value?.accounts ?? [];
  return accounts.length > 1
    ? `Monthly AWS spending for ${accounts.map((account) => account.label).join(' + ')}, with a combined total, forecast and separate account budgets.`
    : "What this deployment's AWS account is charged this month: so far, the forecast for the month, by AWS service and against the monthly budget.";
});

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
