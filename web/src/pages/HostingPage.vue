<template>
  <section class="page space-y-6 py-10 sm:py-14">
    <PageHeader title="Hosting" />

    <p class="text-ink-muted">A monthly tier runs your own servers on NovaWorld and funds OpenNova.</p>

    <Notice v-if="alertText" tone="bad" role="alert">{{ alertText }}</Notice>
    <p v-else-if="tiers === null" role="status" class="text-sm text-ink-muted">Loading the tiers…</p>
    <p v-else-if="!tiers.enabled" class="text-sm text-ink-muted">Hosting is not open yet.</p>

    <template v-else>
      <TierPicker :tiers="tiers.tiers" :currency="tiers.currency" mode="subscribe" selection-only base-id="public-support" @choose="onChoose" />
      <p v-if="tiers.own_machine" id="public-own-machine" class="text-sm text-ink">
        <span class="font-semibold">Dedicated machines:</span>
        <template v-if="tiers.own_machine.amount !== null"> {{ formatMoney(tiers.own_machine.amount, tiers.currency) }} a month each,</template>
        one more server on a machine of its own, no tier needed.
      </p>
    </template>

    <p id="run-it-yourself" class="text-sm text-ink">
      Rather run it yourself? <a :href="SERVE_README_URL" target="_blank" rel="noopener" class="link">Host a server for free</a>.
    </p>

    <p v-if="tiers?.enabled" class="text-xs text-ink-muted">
      Monthly in {{ tiers.currency.toUpperCase() }} until you cancel; a tier supports the project and is not the sale of any game
      (<RouterLink to="/terms" class="link">terms</RouterLink>, <RouterLink to="/refunds" class="link">refunds</RouterLink>).
    </p>
  </section>
</template>

<script setup lang="ts">
// /hosting: the tiers and their prices (GET /api/billing/tiers), for anyone,
// logged in or not. Choosing one goes on to Billing (/me/billing?tier=N),
// through the log in for a visitor who is not logged in.
import { onMounted, ref } from 'vue';
import { RouterLink, useRouter } from 'vue-router';
import TierPicker from '../components/TierPicker.vue';
import Notice from '../components/ui/Notice.vue';
import PageHeader from '../components/ui/PageHeader.vue';
import { fetchTiers } from '../api/billing';
import { useSession } from '../composables/useSession';
import type { TierNumber, TiersResponse } from '../types/billing';
import { formatMoney } from '../utils/format';
import { SERVE_README_URL } from '../utils/links';

const router = useRouter();
const { user } = useSession();

const tiers = ref<TiersResponse | null>(null);
const alertText = ref('');

function onChoose(tier: TierNumber) {
  const destination = `/me/billing?tier=${tier}`;
  if (user.value) void router.push(destination);
  else void router.push({ path: '/login', query: { redirect: destination } });
}

onMounted(async () => {
  const result = await fetchTiers();
  if (result.ok) tiers.value = result.value;
  else alertText.value = result.error.message;
});
</script>
