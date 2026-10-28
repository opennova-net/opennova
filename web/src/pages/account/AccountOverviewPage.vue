<template>
  <div class="space-y-6">
    <PageHeader title="Overview" refreshable :loaded-at="loadedAt" :now="now" :loading="poll.loading.value" @refresh="poll.refresh">
      <template #actions>
        <RouterLink v-if="canStart" to="/me/servers/new" class="btn-primary">Start a server</RouterLink>
      </template>
    </PageHeader>

    <Notice v-if="errors.length" tone="bad" role="alert">{{ errors.join(' ') }}</Notice>
    <Notice v-if="pastDue" tone="warn" class="past-due">
      A payment did not go through. <RouterLink to="/me/billing" class="link">Update your payment method</RouterLink>
    </Notice>

    <dl class="overview-tiles grid grid-cols-1 gap-3 sm:grid-cols-3">
      <StatTile label="Servers" to="/me/servers" :value="serversValue" :detail="serversDetail" />
      <StatTile label="Projects" to="/me/projects" :value="projectsValue" :detail="projectsDetail" />
      <StatTile label="Billing" to="/me/billing" :value="billingValue" :detail="billingDetail" :tone="pastDue ? 'warn' : undefined" />
    </dl>
  </div>
</template>

<script setup lang="ts">
// /me: the account at a glance, a tile each for its servers, projects and
// billing, each opening its page. It reads every 30 seconds; each read stands
// alone.
import { computed, ref } from 'vue';
import { RouterLink, useRoute, useRouter } from 'vue-router';
import StatTile from '../../components/charts/StatTile.vue';
import Notice from '../../components/ui/Notice.vue';
import PageHeader from '../../components/ui/PageHeader.vue';
import { fetchMyServers } from '../../api/hostedServer';
import { fetchMyBilling } from '../../api/billing';
import { fetchMyProjects } from '../../api/products';
import { useSession } from '../../composables/useSession';
import { usePolling } from '../../composables/usePolling';
import type { HostedServersResponse } from '../../types/hostedServer';
import type { MyBilling } from '../../types/billing';
import type { MyProjects } from '../../types/products';
import { startBlock } from '../../utils/myServersView';
import { subscriptionStatusLabel } from '../../utils/billingView';
import { formatDate } from '../../utils/format';

const { refresh: refreshSession } = useSession();
const route = useRoute();
const router = useRouter();
const hosting = ref<HostedServersResponse | null>(null);
const billing = ref<MyBilling | null>(null);
// Billing is off on this deployment (the read answers billing_disabled).
const billingOff = ref(false);
const projects = ref<MyProjects | null>(null);
const errors = ref<string[]>([]);
const loadedAt = ref<number | null>(null);
const now = ref(Date.now());

const live = computed(() => hosting.value?.servers.filter((server) => server.live) ?? []);
const canStart = computed(() => hosting.value !== null && startBlock(hosting.value) === null);
const pastDue = computed(
  () =>
    !!billing.value &&
    [billing.value.subscription, ...billing.value.own_machine_subscriptions, ...billing.value.address_subscriptions].some(
      (subscription) => subscription?.status === 'past_due',
    ),
);

const serversValue = computed(() => (hosting.value ? `${live.value.length} live` : '…'));
const serversDetail = computed(() => {
  const h = hosting.value;
  if (!h) return undefined;
  if (!h.enabled) return 'Hosting is off';
  return h.entitlement.max_servers > 0 ? `${h.entitlement.used} of ${h.entitlement.max_servers} in use` : 'No hosting yet';
});

// The newest project: the one created last (the highest product id).
const newest = computed(() => [...(projects.value?.projects ?? [])].sort((a, b) => b.product_id - a.product_id)[0] ?? null);
const projectsValue = computed(() => (projects.value ? String(projects.value.projects.length) : '…'));
const projectsDetail = computed(() => (newest.value ? `Newest: ${newest.value.title}` : projects.value ? 'No projects yet' : undefined));

const billingValue = computed(() => {
  const b = billing.value;
  if (!b) return '…';
  if (b.subscription) return `Tier ${b.subscription.tier}`;
  return b.granted ? 'Granted' : 'No tier';
});
const billingDetail = computed(() => {
  const b = billing.value;
  if (!b) return undefined;
  const s = b.subscription;
  if (s) {
    const when = s.current_period_end ? `, ${s.cancel_at_period_end ? 'ends' : 'renews'} ${formatDate(s.current_period_end)}` : '';
    return `${subscriptionStatusLabel(s.status)}${when}`;
  }
  if (b.own_machines > 0) return `${b.own_machines} dedicated machine${b.own_machines === 1 ? '' : 's'}`;
  return b.enabled ? undefined : 'Billing is off';
});

const poll = usePolling({
  async read(signal) {
    const [servers, account, mine] = await Promise.all([fetchMyServers(signal), fetchMyBilling(signal), fetchMyProjects(signal)]);
    if (signal.aborted) return false;
    now.value = Date.now();
    const problems: string[] = [];
    if (servers.ok) hosting.value = servers.value;
    else problems.push(servers.error.message);
    billingOff.value = !account.ok && account.error.code === 'billing_disabled';
    if (account.ok) billing.value = account.value;
    else if (!billingOff.value) problems.push(account.error.message);
    if (mine.ok) projects.value = mine.value;
    else problems.push(mine.error.message);
    errors.value = problems.filter((m, i) => problems.indexOf(m) === i);
    loadedAt.value = now.value;
    const unauthorized = [servers, account, mine].some((r) => !r.ok && r.error.code === 'unauthorized');
    if (unauthorized) {
      await refreshSession();
      await router.replace({ path: '/login', query: { redirect: route.fullPath } });
    }
    return problems.length === 0;
  },
  next: () => 30_000,
});
</script>
