<template>
  <div class="space-y-8">
    <PageHeader title="Overview" refreshable :loaded-at="loadedAt" :now="now" :loading="poll.loading.value" @refresh="poll.refresh">
      <template #actions>
        <RouterLink v-if="canStart" to="/me/servers/new" class="btn-primary">Start a server</RouterLink>
      </template>
    </PageHeader>

    <Notice v-if="problems.length" tone="bad" role="alert" class="overview-problems">{{ problemText }}</Notice>

    <dl class="overview-tiles grid grid-cols-1 gap-3 sm:grid-cols-3">
      <StatTile label="Servers" to="/me/servers" :value="serversValue" :detail="serversDetail" />
      <StatTile label="Projects" to="/me/projects" :value="projectsValue" :detail="projectsDetail" />
      <StatTile label="Billing" to="/me/billing" :value="billingValue" :detail="billingDetail" :tone="pastDue ? 'warn' : undefined" />
    </dl>

    <NeedsYouList :items="needs" :now="now" :empty="problems.length ? '' : undefined" />
  </div>
</template>

<script setup lang="ts">
// /me: the account at a glance, a tile each for its servers, projects and
// billing, each opening its page, then what needs the player: a payment past
// due, a server that failed today or has an update, a static address to name,
// a release sent back or awaiting approval. It reads every 30 seconds (the
// releases every 5 minutes, and on Refresh); each read stands alone, so one
// failing leaves the others shown.
import { computed, ref } from 'vue';
import { RouterLink, useRoute, useRouter } from 'vue-router';
import NeedsYouList, { type NeedsYouItem } from '../../components/NeedsYouList.vue';
import StatTile from '../../components/charts/StatTile.vue';
import Notice from '../../components/ui/Notice.vue';
import PageHeader from '../../components/ui/PageHeader.vue';
import { fetchMyAddresses } from '../../api/addresses';
import { fetchMyBilling } from '../../api/billing';
import { fetchMyServers } from '../../api/hostedServer';
import { listMyServers } from '../../api/myServers';
import { fetchMyProjects, fetchProjectReleases } from '../../api/products';
import { useSession } from '../../composables/useSession';
import { usePolling } from '../../composables/usePolling';
import type { AddressesResponse } from '../../types/addresses';
import type { MyBilling } from '../../types/billing';
import type { HostedServersResponse } from '../../types/hostedServer';
import type { MyServer } from '../../types/myServers';
import type { MyProjects, Release } from '../../types/products';
import { hasLiveTier, subscriptionStatusLabel } from '../../utils/billingView';
import { countLabel, formatCount, formatDate, timeMs } from '../../utils/format';
import { replacedServers } from '../../utils/history';
import { startBlock } from '../../utils/myServersView';
import { releaseLabel } from '../../utils/productsView';
import { myProjectLink } from '../../utils/projectsView';

const POLL_MS = 30_000;
const RELEASES_MS = 5 * 60_000;
// A failed server needs its owner for a day.
const FAILED_WINDOW_MS = 24 * 3_600_000;
// The most projects whose releases the page reads.
const RELEASE_PROJECTS_MAX = 10;

const { refresh: refreshSession } = useSession();
const route = useRoute();
const router = useRouter();

const hosting = ref<HostedServersResponse | null>(null);
// The account's listed servers (its hosted ones' listings, and the games it
// hosts itself); null when unread.
const hosts = ref<MyServer[] | null>(null);
const billing = ref<MyBilling | null>(null);
// Billing is off on this deployment (the read answers billing_disabled).
const billingOff = ref(false);
const addresses = ref<AddressesResponse | null>(null);
const projects = ref<MyProjects | null>(null);
// The releases of each project the account may release, by key.
const releases = ref<Record<string, Release[]>>({});
let releasesReadAt = 0;
// What could not be read, and why.
const problems = ref<{ what: string; message: string }[]>([]);
const loadedAt = ref<number | null>(null);
const now = ref(Date.now());

// "Could not read your servers and billing: <why>", one sentence per reason.
const problemText = computed(() => {
  const byMessage = new Map<string, string[]>();
  for (const p of problems.value) byMessage.set(p.message, [...(byMessage.get(p.message) ?? []), p.what]);
  return [...byMessage].map(([message, what]) => `Could not read ${what.join(', ')}: ${message}`).join(' ');
});

// A tile's value before its first answer, or after a failed one.
const unread = computed(() => (loadedAt.value === null ? '…' : 'Not loaded'));

const canStart = computed(() => hosting.value !== null && startBlock(hosting.value) === null);
const live = computed(() => hosting.value?.servers.filter((s) => s.live) ?? []);
const selfHosted = computed(() => (hosts.value ?? []).filter((h) => h.hosted_server_id == null));
// The players on the account's live servers: every listing's, else the hosted
// servers' own.
const players = computed(() =>
  hosts.value ? hosts.value.reduce((n, h) => n + h.players, 0) : live.value.reduce((n, s) => n + (s.listing?.players ?? 0), 0),
);

const serversValue = computed(() => (hosting.value ? `${live.value.length + selfHosted.value.length} live` : unread.value));
const serversDetail = computed(() => {
  const h = hosting.value;
  if (!h) return undefined;
  const count = live.value.length + selfHosted.value.length;
  if (count > 0) return `${countLabel(players.value, 'player')} on ${count === 1 ? 'it' : 'them'}`;
  if (startBlock(h) === 'off') return 'Hosting is off';
  return h.entitlement.max_servers > 0 ? `${h.entitlement.used} of ${h.entitlement.max_servers} in use` : 'No hosting yet';
});

const projectsValue = computed(() => (projects.value ? formatCount(projects.value.projects.length) : unread.value));
const projectsDetail = computed(() => {
  const list = projects.value?.projects ?? [];
  return list.length > 0 ? `${list.filter((p) => p.release !== null).length} published` : undefined;
});

const billingValue = computed(() => {
  const b = billing.value;
  if (billingOff.value || b?.enabled === false) return 'Billing off';
  if (!b) return unread.value;
  if (hasLiveTier(b) && b.subscription) return `Tier ${b.subscription.tier}`;
  return b.granted ? 'Admin grant' : 'No tier';
});
const billingDetail = computed(() => {
  const b = billing.value;
  if (!b || billingOff.value || !b.enabled) return undefined;
  const s = b.subscription;
  if (s && hasLiveTier(b)) {
    const when = s.current_period_end ? `, ${s.cancel_at_period_end ? 'ends' : 'renews'} ${formatDate(s.current_period_end)}` : '';
    return `${subscriptionStatusLabel(s.status)}${when}`;
  }
  return b.own_machines > 0 ? countLabel(b.own_machines, 'dedicated machine') : undefined;
});

// A payment of the account's past due: its billing says so, or its hosting
// refuses a start for it.
const pastDue = computed(() => {
  const b = billing.value;
  if (b && [b.subscription, ...b.own_machine_subscriptions, ...b.address_subscriptions].some((s) => s?.status === 'past_due')) return true;
  return !!hosting.value && startBlock(hosting.value) === 'past_due';
});

// What needs the player, worst first; null before the first answer.
const needs = computed<NeedsYouItem[] | null>(() => {
  if (loadedAt.value === null) return null;
  const items: NeedsYouItem[] = [];
  if (pastDue.value) {
    items.push({ key: 'past-due', tone: 'bad', label: 'Act now', subject: 'Billing', to: '/me/billing', text: 'A payment did not go through.' });
  }
  const servers = hosting.value?.servers ?? [];
  const replaced = replacedServers(servers);
  for (const s of servers) {
    const ended = timeMs(s.ended_at ?? s.updated_at);
    if (s.state !== 'failed' || replaced.has(s.id) || !(now.value - ended < FAILED_WINDOW_MS)) continue;
    items.push({
      key: `failed-${s.id}`,
      tone: 'bad',
      label: 'Act now',
      subject: s.settings.game_name,
      to: `/me/servers/${s.id}`,
      server: { id: s.id, name: s.settings.game_name },
      text: s.failure_reason ? `Failed: ${s.failure_reason}` : 'Failed.',
      since: s.ended_at ?? s.updated_at,
    });
  }
  for (const s of live.value.filter((s) => s.update_available && !s.update_requested)) {
    items.push({
      key: `update-${s.id}`,
      tone: 'warn',
      label: 'Look soon',
      subject: s.settings.game_name,
      to: `/me/servers/${s.id}`,
      server: { id: s.id, name: s.settings.game_name },
      text: 'Update available.',
    });
  }
  const unnamed = addresses.value?.enabled ? addresses.value.unnamed : 0;
  if (unnamed > 0) {
    items.push({
      key: 'unnamed-addresses',
      tone: 'warn',
      label: 'Look soon',
      subject: 'Static addresses',
      to: '/me/billing#addresses',
      text: unnamed === 1 ? 'One waits for a name.' : `${unnamed} wait for a name.`,
    });
  }
  const notes: NeedsYouItem[] = [];
  for (const p of projects.value?.projects ?? []) {
    for (const r of releases.value[p.key] ?? []) {
      const base = { subject: p.title, to: myProjectLink(p.key) };
      if (r.status === 'draft' && r.review_note) {
        items.push({ ...base, key: `sent-back-${r.id}`, tone: 'warn', label: 'Look soon', text: `${releaseLabel(r)} sent back: ${r.review_note}` });
      } else if (r.status === 'awaiting_approval') {
        notes.push({ ...base, key: `awaiting-${r.id}`, tone: 'muted', label: 'Note', text: `${releaseLabel(r)} awaits approval.` });
      }
    }
  }
  return [...items, ...notes];
});

async function onSessionEnded() {
  await refreshSession();
  await router.replace({ path: '/login', query: { redirect: route.fullPath } });
}

const poll = usePolling({
  async read(signal, background) {
    const [servers, listed, account, held, mine] = await Promise.all([
      fetchMyServers(signal),
      listMyServers(signal),
      fetchMyBilling(signal),
      fetchMyAddresses(signal),
      fetchMyProjects(signal),
    ]);
    if (signal.aborted) return false;
    if ([servers, listed, account, held, mine].some((r) => !r.ok && r.error.code === 'unauthorized')) {
      await onSessionEnded();
      return false;
    }
    const out: { what: string; message: string }[] = [];
    if (servers.ok) hosting.value = servers.value;
    else out.push({ what: 'your servers', message: servers.error.message });
    // The listings only add players and the games hosted from the game: their
    // failure says nothing.
    if (listed.ok) hosts.value = listed.value;
    billingOff.value = !account.ok && account.error.code === 'billing_disabled';
    if (account.ok) billing.value = account.value;
    else if (!billingOff.value) out.push({ what: 'billing', message: account.error.message });
    if (held.ok) addresses.value = held.value;
    else if (held.error.code !== 'addresses_disabled') out.push({ what: 'your static addresses', message: held.error.message });
    if (mine.ok) projects.value = mine.value;
    else out.push({ what: 'your projects', message: mine.error.message });

    if (mine.ok && (!background || Date.now() - releasesReadAt >= RELEASES_MS)) {
      const keys = mine.value.projects
        .filter((p) => p.can_release && p.head > 0)
        .slice(0, RELEASE_PROJECTS_MAX)
        .map((p) => p.key);
      const lists = await Promise.all(keys.map((key) => fetchProjectReleases(key, signal)));
      if (signal.aborted) return false;
      if (lists.some((r) => !r.ok && r.error.code === 'unauthorized')) {
        await onSessionEnded();
        return false;
      }
      const next: Record<string, Release[]> = {};
      let failed = '';
      keys.forEach((key, i) => {
        const r = lists[i];
        if (r.ok) next[key] = r.value;
        else {
          failed = r.error.message;
          if (releases.value[key]) next[key] = releases.value[key];
        }
      });
      releases.value = next;
      if (failed) out.push({ what: 'your releases', message: failed });
      else releasesReadAt = Date.now();
    }

    now.value = Date.now();
    problems.value = out;
    loadedAt.value = now.value;
    return out.length === 0;
  },
  next: () => POLL_MS,
});
</script>
