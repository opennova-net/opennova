<template>
  <div class="space-y-8">
    <header class="flex flex-wrap items-end justify-between gap-4">
      <div>
        <h1 class="page-heading">Welcome back{{ user ? `, ${user.username}` : '' }}.</h1>
        <p class="page-lead">Find your next game or pick up where your servers left off.</p>
      </div>
      <button type="button" class="btn-secondary" :disabled="poll.loading.value" @click="poll.refresh">{{ poll.loading.value ? 'Refreshing…' : 'Refresh' }}</button>
    </header>

    <section class="rounded-panel border border-border bg-panel p-6 sm:p-8" aria-labelledby="account-play">
      <p class="eyebrow">Ready when you are</p>
      <h2 id="account-play" class="mt-3 text-2xl font-semibold text-ink-bright">A good game starts here.</h2>
      <p class="mt-2 max-w-xl text-sm leading-relaxed text-ink-muted">See who is playing on NovaWorld. Your account works on the website and in supported games.</p>
      <div class="mt-6 flex flex-wrap gap-3">
        <RouterLink to="/lobby" class="btn-primary">Find a server</RouterLink>
        <RouterLink to="/downloads" class="btn-secondary">Get OpenNova</RouterLink>
      </div>
    </section>

    <p v-if="errors.length" role="alert" class="notice border-danger/40 bg-danger/10 text-danger">{{ errors.join(' ') }}</p>

    <div class="grid gap-5 xl:grid-cols-2">
      <section class="rounded-panel border border-border bg-panel p-5 sm:p-6" aria-labelledby="account-hosting">
        <div class="flex items-center justify-between gap-3">
          <h2 id="account-hosting" class="section-heading">Your servers</h2>
          <RouterLink to="/me/servers" class="text-sm text-accent">View all <span aria-hidden="true">→</span></RouterLink>
        </div>
        <template v-if="hosting">
          <p class="mt-4 text-3xl font-semibold text-ink-bright">{{ live.length }} <span class="text-sm font-normal text-ink-muted">active</span></p>
          <p class="mt-2 text-sm text-ink-muted">{{ hosting.entitlement.used }} of {{ hosting.entitlement.max_servers }} hosting places in use.</p>
          <ul v-if="live.length" class="mt-5 divide-y divide-border">
            <li v-for="server in live.slice(0, 3)" :key="server.id">
              <RouterLink :to="`/me/servers/${server.id}`" class="flex items-center justify-between gap-3 py-3 text-sm hover:text-accent">
                <span class="min-w-0 truncate font-medium">{{ server.settings.game_name || 'Unnamed server' }}</span>
                <span :class="stateBadgeClass(server.state)">{{ ownerStateLabel(server.state) }}</span>
              </RouterLink>
            </li>
          </ul>
          <p v-else class="mt-4 text-sm text-ink-muted">No active servers. You can still join any public game.</p>
          <RouterLink v-if="canStart" to="/me/servers/new" class="btn-primary mt-5">Start a server</RouterLink>
          <RouterLink v-else-if="!hosting.entitlement.max_servers && hosting.enabled" to="/me/support" class="btn-secondary mt-5">Explore hosting</RouterLink>
          <p v-if="!hosting.enabled" class="mt-4 text-sm text-ink-muted">New hosted servers are unavailable right now.</p>
        </template>
        <p v-else class="mt-4 text-sm text-ink-muted">{{ poll.loading.value ? 'Loading your servers…' : 'Your servers could not be read.' }}</p>
      </section>

      <section class="rounded-panel border border-border bg-panel p-5 sm:p-6" aria-labelledby="account-billing">
        <div class="flex items-center justify-between gap-3">
          <h2 id="account-billing" class="section-heading">Hosting & billing</h2>
          <RouterLink to="/me/support" class="text-sm text-accent">View billing <span aria-hidden="true">→</span></RouterLink>
        </div>
        <template v-if="billing">
          <p class="mt-5 text-xl font-semibold text-ink">{{ billing.granted ? 'Hosting included' : billing.subscription ? `Tier ${billing.subscription.tier}` : 'No support tier' }}</p>
          <p v-if="billing.subscription" class="mt-2 text-sm text-ink-muted">{{ subscriptionStatusLabel(billing.subscription.status) }}<template v-if="billing.subscription.current_period_end"> · {{ billing.subscription.cancel_at_period_end ? 'Ends' : 'Renews' }} {{ formatDate(billing.subscription.current_period_end) }}</template></p>
          <p v-else class="mt-2 text-sm text-ink-muted">{{ billing.granted ? 'Your account has hosting granted by an admin.' : 'Playing on NovaWorld is free. Support tiers add hosted servers.' }}</p>
          <p v-if="billing.own_machines" class="mt-4 text-sm text-ink-muted">{{ billing.own_machines }} dedicated machine{{ billing.own_machines === 1 ? '' : 's' }}</p>
          <RouterLink v-if="pastDue" to="/me/support" class="mt-5 block rounded-control border border-warn/40 bg-warn/10 p-3 text-sm text-warn">A payment needs attention. Update your payment method →</RouterLink>
          <p v-if="!billing.enabled" class="mt-4 text-sm text-ink-muted">New subscriptions are unavailable right now.</p>
        </template>
        <p v-else class="mt-4 text-sm text-ink-muted">{{ poll.loading.value ? 'Loading your billing…' : 'Billing could not be read.' }}</p>
      </section>
    </div>
  </div>
</template>

<script setup lang="ts">
import { computed, ref } from 'vue';
import { RouterLink, useRoute, useRouter } from 'vue-router';
import { fetchMyServers } from '../../api/hostedServer';
import { fetchMyBilling } from '../../api/billing';
import { useSession } from '../../composables/useSession';
import { usePolling } from '../../composables/usePolling';
import type { HostedServersResponse } from '../../types/hostedServer';
import type { MyBilling } from '../../types/billing';
import { startBlock } from '../../utils/myServersView';
import { ownerStateLabel, stateBadgeClass } from '../../utils/hostedState';
import { subscriptionStatusLabel } from '../../utils/billingView';
import { formatDate } from '../../utils/format';

const { user, refresh: refreshSession } = useSession();
const route = useRoute();
const router = useRouter();
const hosting = ref<HostedServersResponse | null>(null);
const billing = ref<MyBilling | null>(null);
const errors = ref<string[]>([]);
const live = computed(() => hosting.value?.servers.filter((server) => server.live) ?? []);
const canStart = computed(() => hosting.value !== null && startBlock(hosting.value) === null);
const pastDue = computed(() => billing.value && [billing.value.subscription, ...billing.value.own_machine_subscriptions, ...billing.value.address_subscriptions].some((subscription) => subscription?.status === 'past_due'));
const poll = usePolling({
  async read(signal) {
    const [servers, account] = await Promise.all([fetchMyServers(signal), fetchMyBilling(signal)]);
    if (signal.aborted) return false;
    const problems: string[] = [];
    if (servers.ok) hosting.value = servers.value;
    else problems.push(servers.error.message);
    if (account.ok) billing.value = account.value;
    else problems.push(account.error.message);
    errors.value = problems;
    if ((!servers.ok && servers.error.code === 'unauthorized') || (!account.ok && account.error.code === 'unauthorized')) {
      await refreshSession();
      await router.replace({ path: '/login', query: { redirect: route.fullPath } });
    }
    return problems.length === 0;
  },
  next: () => 30_000,
});
</script>
