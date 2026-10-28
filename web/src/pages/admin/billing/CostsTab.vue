<template>
  <div class="space-y-8">
    <PageHeader title="Billing" :tabs="BILLING_TABS" refreshable :loaded-at="loadedAt" :now="now" :loading="poll.loading.value" @refresh="poll.refresh">
      <template v-if="costs?.available" #meta>
        <span v-if="scope" class="cost-scope">{{ scope }}</span>
        <span v-if="costs.read_at">Read from AWS <RelativeTime :value="costs.read_at" :now="now" /></span>
        <Badge v-if="costs.estimated" title="AWS closes the month's figures after it ends">Estimate</Badge>
      </template>
    </PageHeader>

    <LiveNotices :error="error" />
    <p v-if="costs === null && !error" class="text-sm text-ink-muted">Loading…</p>

    <CostSummary v-if="costs" :costs="costs" />
  </div>
</template>

<script setup lang="ts">
// /admin/billing/costs: the configured AWS accounts' bill this month. The
// service keeps a read for hours, so the page reads once and on Refresh.
import { computed, ref } from 'vue';
import CostSummary from '../../../components/CostSummary.vue';
import RelativeTime from '../../../components/RelativeTime.vue';
import Badge from '../../../components/ui/Badge.vue';
import LiveNotices from '../../../components/ui/LiveNotices.vue';
import PageHeader from '../../../components/ui/PageHeader.vue';
import { fetchCosts } from '../../../api/costs';
import { usePolling } from '../../../composables/usePolling';
import type { Costs } from '../../../types/costs';
import { BILLING_TABS } from '../../../utils/adminNav';

const costs = ref<Costs | null>(null);
const error = ref('');
const loadedAt = ref<number | null>(null);
const now = ref(Date.now());

// "Production + CI" when several accounts add up.
const scope = computed(() => {
  const accounts = costs.value?.accounts ?? [];
  return accounts.length > 1 ? accounts.map((a) => a.label).join(' + ') : '';
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
