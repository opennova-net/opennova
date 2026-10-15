<template>
  <div class="space-y-8">
    <header class="space-y-2">
      <RouterLink to="/me/servers" class="text-sm text-accent hover:text-ink-bright">All servers</RouterLink>
      <h1 ref="heading" tabindex="-1" class="text-3xl font-semibold text-ink focus:outline-none">
        {{ from ? `Start ${from.settings.game_name || 'your server'} again` : 'New server' }}
      </h1>
    </header>

    <p role="alert" class="sr-only">{{ notice?.message }}</p>
    <p v-if="notice" class="rounded-panel border border-danger/40 bg-danger/10 p-4 text-sm text-danger">
      <span aria-hidden="true">{{ notice.message }}</span>
      <RouterLink v-if="linksToSupportTier(notice.code)" to="/me/support" class="ml-1 text-accent hover:text-ink-bright">
        See your support tier
      </RouterLink>
    </p>

    <div v-if="data === null && !loadError" class="animate-pulse rounded-panel border border-border bg-panel p-8 text-center text-ink-muted">
      Loading…
    </div>

    <div v-else-if="data === null" class="space-y-3 rounded-panel border border-border bg-panel p-6">
      <p class="text-sm text-danger">{{ loadError }}</p>
      <button
        type="button"
        class="rounded-control border border-border bg-raised px-4 py-2 text-sm font-medium text-ink transition hover:bg-hover"
        @click="load"
      >
        Try again
      </button>
    </div>

    <section v-else-if="block !== null" class="space-y-3 rounded-panel border border-border bg-panel p-6" aria-labelledby="new-server-blocked">
      <h2 id="new-server-blocked" class="text-lg font-semibold text-ink">No new server can start right now</h2>
      <p class="text-sm text-ink-muted">{{ blockText }}</p>
      <p class="flex flex-wrap gap-4 text-sm">
        <RouterLink
          v-if="block === 'locked' || block === 'past_due' || (block === 'limit' && data.entitlement.source !== 'comp')"
          to="/me/support"
          class="text-accent hover:text-ink-bright"
        >
          {{ block === 'past_due' ? 'Update your payment method' : 'See the support tiers' }}
        </RouterLink>
        <RouterLink to="/me/servers" class="text-accent hover:text-ink-bright">Back to your servers</RouterLink>
      </p>
    </section>

    <template v-else>
      <p v-if="fromGone" class="rounded-panel border border-warn/40 bg-warn/10 p-4 text-sm text-warn">
        The server you started from is no longer listed, so the form begins from the defaults.
      </p>
      <HostedSettingsForm
        base-id="new-server"
        :previous="from?.settings ?? null"
        :data-sets="data.data_sets"
        :retry="from?.state === 'failed'"
        :own-machines-free="ownMachinesFree(data.entitlement)"
        @started="onStarted"
        @refused="onRefused"
      />
    </template>
  </div>
</template>

<script setup lang="ts">
// /me/servers/new?from=<id>: a new hosted server's settings and its start
// (HostedSettingsForm, POST /api/me/servers), offered only while the
// account's entitlement has room (GET /api/me/servers). `from` names one of
// the account's ended servers to start again: the form begins with its
// settings (its try again, after a failure). A start that went in goes to the
// new server's page.
import { computed, onMounted, ref } from 'vue';
import { RouterLink, useRoute, useRouter } from 'vue-router';
import HostedSettingsForm from '../../components/HostedSettingsForm.vue';
import { fetchMyServer, fetchMyServers, linksToSupportTier, type HostedError } from '../../api/hostedServer';
import { useSession } from '../../composables/useSession';
import type { HostedServer, HostedServersResponse } from '../../types/hostedServer';
import { ownMachinesFree, startBlock } from '../../utils/myServersView';

const route = useRoute();
const router = useRouter();
const { refresh: refreshSession } = useSession();

const data = ref<HostedServersResponse | null>(null);
const from = ref<HostedServer | null>(null);
const fromGone = ref(false);
const loadError = ref('');
// The refusal that took the form away, kept above what replaced it.
const notice = ref<HostedError | null>(null);

const block = computed(() => (data.value ? startBlock(data.value) : null));

const blockText = computed(() => {
  switch (block.value) {
    case 'off':
      return 'NovaWorld is not starting hosted servers right now.';
    case 'locked':
      return 'Hosting a server needs a support tier.';
    case 'past_due':
      return "Your support tier's last payment did not go through. Your servers keep running while Stripe retries the card; new servers can't start.";
    case 'limit':
      return 'All the servers you may run at once are live. Stop one first.';
    case 'full':
      return 'Every hosted server place on NovaWorld is taken right now. Try again later.';
    default:
      return '';
  }
});

// The `from` query as a server id, or null.
function fromId(): number | null {
  const raw = route.query.from;
  const text = Array.isArray(raw) ? raw[0] : raw;
  if (typeof text !== 'string' || !/^\d+$/.test(text)) return null;
  return Number(text);
}

async function onSessionEnded() {
  await refreshSession();
  await router.replace({ path: '/login', query: { redirect: route.fullPath } });
}

async function load() {
  loadError.value = '';
  const result = await fetchMyServers();
  if (!result.ok) {
    if (result.error.code === 'unauthorized') return onSessionEnded();
    loadError.value = result.error.message;
    return;
  }
  const id = fromId();
  if (id !== null && from.value?.id !== id) {
    const listed = result.value.servers.find((s) => s.id === id);
    if (listed) {
      from.value = listed;
    } else {
      // Older than the list's newest ended ones: read it on its own.
      const one = await fetchMyServer(id);
      if (one.ok) from.value = one.value;
      else fromGone.value = true;
    }
  }
  data.value = result.value;
}

function onStarted(server: HostedServer) {
  void router.push(`/me/servers/${server.id}`);
}

// A refusal that means the page's picture is out of date (the entitlement,
// the payment or hosting changed): read it again, which takes the form away
// when no start is allowed, so the refusal's sentence stays above it. An own
// machine taken meanwhile only takes the checkbox away (the form says why).
function onRefused(error: HostedError) {
  if (error.code === 'unauthorized') {
    void onSessionEnded();
    return;
  }
  if (error.code === 'no_own_machine') {
    void load();
    return;
  }
  if (
    error.code === 'not_entitled' ||
    error.code === 'payment_past_due' ||
    error.code === 'server_limit' ||
    error.code === 'hosting_disabled' ||
    error.code === 'at_capacity'
  ) {
    notice.value = error;
    void load();
  }
}

onMounted(load);
</script>
