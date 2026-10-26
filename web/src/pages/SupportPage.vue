<template>
  <section class="page py-10 sm:py-14">
    <div class="space-y-10">
      <header class="space-y-4">
        <p class="eyebrow">Server hosting</p>
        <h1 class="page-heading">Your server. Your next game night.</h1>
        <p class="max-w-3xl text-base text-ink-muted">
          Support OpenNova and give your group a place to play. We run your server and list it on NovaWorld;
          you choose the game, missions and settings. It stays up until you stop it.
        </p>
        <p class="max-w-3xl text-base text-ink-muted">
          A Joint Operations server can also list on NovaLogic's NovaWorld, reaching players on the original game with nothing to install.
          Retail and OpenNova players can join. Hosting uses OpenNova's experimental server and may differ from the
          original game. A tier supports the project; it is not the sale of any game.
        </p>
      </header>

      <p v-if="alertText" role="alert" class="rounded-panel border border-danger/40 bg-danger/10 p-4 text-sm text-danger">
        {{ alertText }}
      </p>

      <div v-if="tiers === null && !alertText" class="animate-pulse rounded-panel border border-border bg-panel p-8 text-center text-ink-muted">
        Loading the support tiers…
      </div>

      <p v-else-if="tiers && !tiers.enabled" class="rounded-panel border border-border bg-panel p-6 text-sm text-ink-muted">
        Support tiers are not open on NovaWorld yet.
      </p>

      <template v-else-if="tiers">
        <section class="space-y-4" aria-labelledby="public-support-tiers">
          <h2 id="public-support-tiers" class="text-xl font-semibold text-ink">Choose a tier</h2>
          <TierPicker :tiers="tiers.tiers" :currency="tiers.currency" mode="subscribe" selection-only base-id="public-support" @choose="onChoose" />
          <p class="text-xs text-ink-muted">
            Prices are monthly in {{ tiers.currency.toUpperCase() }} and renew each month until you cancel, which you
            can do at any time in Stripe's billing portal.
            <template v-if="!user">Log in to review your selection before checkout.</template>
            Read the <RouterLink to="/terms" class="link">terms of service</RouterLink>
            and the <RouterLink to="/refunds" class="link">refund policy</RouterLink>
            before you subscribe.
          </p>
        </section>

        <section v-if="tiers.own_machine" class="space-y-2 rounded-panel border border-border bg-panel p-4 sm:p-6" aria-labelledby="public-own-machine">
          <h2 id="public-own-machine" class="text-lg font-semibold text-ink">Dedicated machines</h2>
          <p class="text-sm text-ink-muted">
            <template v-if="tiers.own_machine.amount !== null">
              {{ formatMoney(tiers.own_machine.amount, tiers.currency) }} a month for each.
            </template>
            One more hosted server, on a machine of its own. No support tier needed; add as many as you like, each its
            own monthly subscription, from your support tier page.
          </p>
        </section>
      </template>

      <RunItYourself id="support-run-it-yourself" />
    </div>
  </section>
</template>

<script setup lang="ts">
// /support: the support tiers and their prices (GET /api/billing/tiers), for
// anyone, logged in or not. Subscribe sends a visitor who is not logged in to
// log in first and on to their support tier page (/me/support), where the
// checkout starts; a logged-in visitor goes there at once.
import { onMounted, ref } from 'vue';
import { RouterLink, useRouter } from 'vue-router';
import RunItYourself from '../components/RunItYourself.vue';
import TierPicker from '../components/TierPicker.vue';
import { fetchTiers } from '../api/billing';
import { useSession } from '../composables/useSession';
import type { TierNumber, TiersResponse } from '../types/billing';
import { formatMoney } from '../utils/format';

const router = useRouter();
const { user } = useSession();

const tiers = ref<TiersResponse | null>(null);
const alertText = ref('');

function onChoose(tier: TierNumber) {
  const destination = `/me/support?tier=${tier}`;
  if (user.value) void router.push(destination);
  else void router.push({ path: '/login', query: { redirect: destination } });
}

onMounted(async () => {
  const result = await fetchTiers();
  if (result.ok) tiers.value = result.value;
  else alertText.value = result.error.message;
});
</script>
