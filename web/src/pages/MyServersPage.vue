<template>
  <section class="px-6 pt-24 pb-16">
    <div class="mx-auto max-w-6xl space-y-8">
      <header class="flex flex-col gap-4 sm:flex-row sm:items-end sm:justify-between">
        <div>
          <p class="text-sm uppercase tracking-[0.3em] text-accent">NovaWorld</p>
          <h1 class="text-3xl font-semibold text-ink">My servers</h1>
          <p class="mt-1 text-sm text-ink-muted">
            The game servers listed under
            <span class="font-medium text-ink">{{ user?.username ?? 'your account' }}</span>.
            Commands go to the server over its own NovaWorld session and act on that server alone.
          </p>
        </div>
        <div class="flex items-center gap-3">
          <p v-if="loadedAt" class="text-xs text-ink-muted">
            Updated <time :datetime="new Date(loadedAt).toISOString()">{{ new Date(loadedAt).toLocaleTimeString() }}</time>
          </p>
          <button
            type="button"
            class="rounded-control border border-border bg-raised px-4 py-2 text-sm font-medium text-ink transition hover:bg-hover disabled:cursor-not-allowed disabled:opacity-40"
            :disabled="loading"
            @click="load(false)"
          >
            {{ loading ? 'Refreshing…' : 'Refresh' }}
          </button>
        </div>
      </header>

      <!-- Live regions, always in the page so a screen reader hears what lands in them. -->
      <p role="status" class="sr-only">{{ pageStatus }}</p>
      <p role="alert" class="sr-only">{{ pageError }}</p>
      <p v-if="pageStatus" aria-hidden="true" class="rounded-panel border border-online/40 bg-online/10 p-4 text-sm text-online">
        {{ pageStatus }}
      </p>
      <p v-if="pageError" aria-hidden="true" class="rounded-panel border border-danger/40 bg-danger/10 p-4 text-sm text-danger">
        {{ pageError }}
      </p>

      <div
        v-if="servers === null && loading"
        class="animate-pulse rounded-panel border border-border bg-panel p-8 text-center text-ink-muted"
      >
        Loading your servers…
      </div>

      <div v-else-if="servers !== null && servers.length === 0" class="space-y-3 rounded-panel border border-border bg-panel p-8">
        <h2 class="text-lg font-semibold text-ink">No servers are listed under your account</h2>
        <p class="text-sm text-ink-muted">
          A server is linked to your account when it hosts while logged in to NovaWorld as
          <span class="font-medium text-ink">{{ user?.username ?? 'you' }}</span>: host a game from the
          OpenNova game logged in with this account, or run
          <code class="rounded-control bg-raised px-1 font-mono text-xs text-ink">opennova-serve --credentials &lt;file&gt;</code>
          with this account's NOVAWORLD_USER and NOVAWORLD_PASS.
        </p>
        <p class="text-sm text-ink-muted">
          The link appears once the server's mission starts: a game client first sends NovaWorld its
          host key then, so a server still in its lobby is listed but not yet yours. A server hosted
          without logging in (or <code class="rounded-control bg-raised px-1 font-mono text-xs text-ink">opennova-serve</code>
          without <code class="rounded-control bg-raised px-1 font-mono text-xs text-ink">--credentials</code>)
          is never linked to an account.
        </p>
        <p class="text-sm text-ink-muted">This page refreshes itself every {{ POLL_SECONDS }} seconds.</p>
      </div>

      <div v-else-if="servers !== null" class="space-y-8">
        <MyServerCard
          v-for="server in servers"
          :key="server.rid"
          :server="server"
          :now="now"
          @changed="refreshSoon"
          @stopped="onStopped"
          @session-ended="onSessionEnded"
        />
      </div>
    </div>
  </section>
</template>

<script setup lang="ts">
import { onMounted, onUnmounted, ref } from 'vue';
import { useRoute, useRouter } from 'vue-router';
import MyServerCard from '../components/MyServerCard.vue';
import { listMyServers } from '../api/myServers';
import { useSession } from '../composables/useSession';
import type { MyServer } from '../types/myServers';

const { user, refresh: refreshSession } = useSession();
const route = useRoute();
const router = useRouter();

// Null until the first list answered.
const servers = ref<MyServer[] | null>(null);
const loading = ref(false);
const loadedAt = ref<number | null>(null);
const now = ref(Date.now());
const pageStatus = ref('');
const pageError = ref('');

// The list refreshes itself on this period (a server's columns and roster
// come with its own updates), and on demand.
const POLL_SECONDS = 10;
let pollTimer: ReturnType<typeof setInterval> | undefined;
let soonTimer: ReturnType<typeof setTimeout> | undefined;
let inFlight: AbortController | null = null;

async function onSessionEnded() {
  await refreshSession();
  await router.replace({ path: '/login', query: { redirect: route.fullPath } });
}

// `background` keeps the list (and says nothing) on a failed poll; an asked-for
// refresh shows the failure.
async function load(background: boolean) {
  if (inFlight) {
    if (background) return;
    inFlight.abort();
  }
  const controller = new AbortController();
  inFlight = controller;
  if (!background) loading.value = true;
  const result = await listMyServers(controller.signal);
  if (inFlight !== controller) return; // a newer load took over
  inFlight = null;
  loading.value = false;
  now.value = Date.now();
  if (result.ok) {
    servers.value = result.value;
    loadedAt.value = now.value;
    if (pageError.value && !background) pageError.value = '';
    return;
  }
  if (result.error.code === 'unauthorized') {
    await onSessionEnded();
    return;
  }
  if (!background || servers.value === null) pageError.value = result.error.message;
}

// After a push: the server's own update carries most changes, so look again
// shortly.
function refreshSoon() {
  if (soonTimer) clearTimeout(soonTimer);
  soonTimer = setTimeout(() => void load(true), 1500);
}

function onStopped(name: string) {
  pageStatus.value = `Stopped hosting ${name || 'the server'}. It leaves the list as the match ends.`;
  pageError.value = '';
  void load(false);
}

onMounted(() => {
  void load(false);
  pollTimer = setInterval(() => {
    now.value = Date.now();
    void load(true);
  }, POLL_SECONDS * 1000);
});

onUnmounted(() => {
  if (pollTimer) clearInterval(pollTimer);
  if (soonTimer) clearTimeout(soonTimer);
  // Its answer, when it comes, then finds a newer load in charge and is dropped.
  inFlight?.abort();
  inFlight = null;
});
</script>
