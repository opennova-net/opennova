<template>
  <div class="space-y-10">
    <div class="space-y-2">
      <AdminCrumbs />
      <header class="flex flex-col gap-4 sm:flex-row sm:items-center sm:justify-between">
        <div>
          <h1 class="text-3xl font-semibold text-ink">Status and sessions</h1>
          <p class="text-sm text-ink-muted">
            The service retail players sign in to: whether they can sign in now, and who is signed in. Its lobby
            instances are on <RouterLink to="/admin/infrastructure" class="text-accent hover:text-ink-bright">Infrastructure</RouterLink>,
            the Delta Force Xtreme 2 keys on <RouterLink to="/admin/cd-keys" class="text-accent hover:text-ink-bright">CD keys</RouterLink>.
          </p>
        </div>
        <button type="button" :class="secondaryButton" :disabled="refreshing" @click="refresh">
          {{ refreshing ? 'Refreshing…' : 'Refresh' }}
        </button>
      </header>
    </div>

    <NovaWorldSignIn ref="signIn" />
    <div id="sessions" class="scroll-mt-20">
      <NovaWorldSessions ref="sessions" />
    </div>
  </div>
</template>

<script setup lang="ts">
// /admin/novaworld: the NovaWorld service as retail players meet it, the old
// service and sessions pages in one (/admin/service and /admin/sessions lead
// here): its sign-in first (open or in maintenance, and maintenance mode with
// its message: NovaWorldSignIn), then every session signed in now
// (NovaWorldSessions, #sessions). Refresh reads both again.
import { computed, ref } from 'vue';
import { RouterLink } from 'vue-router';
import AdminCrumbs from '../../components/AdminCrumbs.vue';
import NovaWorldSessions from '../../components/NovaWorldSessions.vue';
import NovaWorldSignIn from '../../components/NovaWorldSignIn.vue';

const secondaryButton =
  'rounded-control border border-border bg-raised px-4 py-2 text-sm font-medium text-ink transition hover:bg-hover disabled:cursor-not-allowed disabled:opacity-40';

const signIn = ref<InstanceType<typeof NovaWorldSignIn> | null>(null);
const sessions = ref<InstanceType<typeof NovaWorldSessions> | null>(null);
const refreshing = computed(() => !!signIn.value?.loading || !!sessions.value?.loading);

function refresh() {
  void signIn.value?.refresh();
  void sessions.value?.refresh();
}
</script>
