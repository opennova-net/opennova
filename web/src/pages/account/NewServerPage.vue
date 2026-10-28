<template>
  <div class="space-y-6">
    <PageHeader :title="from ? `Start ${from.settings.game_name || 'your server'} again` : 'New server'" :crumbs="[{ label: 'Your servers', to: '/me/servers' }]" />

    <p role="alert" class="sr-only">{{ notice?.message }}</p>
    <Notice v-if="notice" tone="bad" class="new-server-refused">
      <span aria-hidden="true">{{ notice.message }}</span>
      <RouterLink v-if="linksToSupportTier(notice.code)" to="/me/billing" class="link ml-1">Billing</RouterLink>
    </Notice>

    <p v-if="data === null && !loadError" class="text-sm text-ink-muted">Loading…</p>

    <Notice v-else-if="data === null" tone="bad">
      {{ loadError }} <button type="button" class="btn-ghost btn-sm" @click="load">Try again</button>
    </Notice>

    <Notice v-else-if="block !== null" id="new-server-blocked" class="flex flex-wrap items-center gap-x-3 gap-y-1">
      <span>{{ blockLine(block) }}</span>
      <RouterLink v-if="link" :to="link.to" class="link">{{ link.label }}</RouterLink>
      <RouterLink to="/me/servers" class="link">Your servers</RouterLink>
    </Notice>

    <template v-else>
      <Notice v-if="fromGone" tone="warn">That server is gone; starting from the defaults.</Notice>
      <HostedSettingsForm
        base-id="new-server"
        :previous="from?.settings ?? null"
        :products="data.products"
        :novalogic-listing="data.novalogic_listing"
        :retry="from?.state === 'failed'"
        :shared-free="sharedFree(data.entitlement)"
        :own-machines-free="ownMachinesFree(data.entitlement)"
        :own-machines="data.entitlement.own_machines"
        :previous-own-machine="from?.own_machine ?? false"
        :addresses="free"
        :preferred-address-id="preferredAddress(free, data.servers, from)"
        @started="onStarted"
        @refused="onRefused"
      />
    </template>
  </div>
</template>

<script setup lang="ts">
// /me/servers/new?from=<id>: a new hosted server's settings and its start
// (HostedSettingsForm, POST /api/me/servers), offered only while the
// account's entitlement has room on shared machines or on a dedicated
// machine (GET /api/me/servers), the form picking between them. `from` names
// one of the account's ended servers to start again: the form begins with
// its settings (its try again, after a failure), on a dedicated machine when
// it ran on one. On a dedicated machine the form offers the account's free
// static addresses (GET /api/me/addresses: its own, ready, no live server's),
// the one `from` (else the newest server) ran at first, so a server started
// again keeps its dedicated machine and its address. A start that went in
// goes to the new server's page.
import { computed, onMounted, ref } from 'vue';
import { RouterLink, useRoute, useRouter } from 'vue-router';
import HostedSettingsForm from '../../components/HostedSettingsForm.vue';
import Notice from '../../components/ui/Notice.vue';
import PageHeader from '../../components/ui/PageHeader.vue';
import { fetchMyAddresses } from '../../api/addresses';
import { fetchMyServer, fetchMyServers, linksToSupportTier, type HostedError } from '../../api/hostedServer';
import { useSession } from '../../composables/useSession';
import type { Address } from '../../types/addresses';
import type { HostedServer, HostedServersResponse } from '../../types/hostedServer';
import { freeAddresses, preferredAddress } from '../../utils/addressView';
import { blockLine, blockLink, ownMachinesFree, sharedFree, startBlock } from '../../utils/myServersView';

const route = useRoute();
const router = useRouter();
const { refresh: refreshSession } = useSession();

const data = ref<HostedServersResponse | null>(null);
// The static addresses a new server may take; none when the read failed (the
// form then offers none).
const free = ref<Address[]>([]);
const from = ref<HostedServer | null>(null);
const fromGone = ref(false);
const loadError = ref('');
// The refusal that took the form away, kept above what replaced it.
const notice = ref<HostedError | null>(null);

const block = computed(() => (data.value ? startBlock(data.value) : null));

const link = computed(() => (data.value && block.value ? blockLink(block.value, data.value.entitlement) : null));

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
  const [result, addresses] = await Promise.all([fetchMyServers(), fetchMyAddresses()]);
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
  free.value = addresses.ok ? freeAddresses(addresses.value.addresses) : [];
  data.value = result.value;
}

function onStarted(server: HostedServer) {
  void router.push(`/me/servers/${server.id}`);
}

// A refusal that means the page's picture is out of date (the entitlement,
// the payment or hosting changed): read it again, which takes the form away
// when no start is allowed, so the refusal's sentence stays above it. A
// dedicated machine or a static address taken meanwhile only takes it from
// the form (the form says why).
function onRefused(error: HostedError) {
  if (error.code === 'unauthorized') {
    void onSessionEnded();
    return;
  }
  if (error.code === 'no_own_machine' || error.code === 'address_unavailable') {
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
