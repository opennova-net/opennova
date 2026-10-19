<template>
  <div class="space-y-10">
    <AdminPageHeader title="Status and sessions" :loading="refreshing" @refresh="refresh">
      <template #description>
        The service retail players sign in to: whether they can sign in now, who is signed in (the clients and the servers they
        play on) and the lobbies that hold them. The lobby program's tasks are on
        <RouterLink to="/admin/infrastructure" class="text-accent hover:text-ink-bright">Infrastructure</RouterLink>, the Delta
        Force Xtreme 2 keys on <RouterLink to="/admin/cd-keys" class="text-accent hover:text-ink-bright">CD keys</RouterLink>.
      </template>
    </AdminPageHeader>

    <NovaWorldSignIn ref="signIn" />
    <div id="sessions" class="scroll-mt-20">
      <NovaWorldSessions ref="sessions" />
    </div>
    <div id="lobbies" class="scroll-mt-20">
      <NovaWorldLobbies ref="lobbies" />
    </div>
  </div>
</template>

<script setup lang="ts">
// /admin/novaworld: the NovaWorld service as retail players meet it, the old
// service and sessions pages in one (/admin/service and /admin/sessions lead
// here): its sign-in first (open or in maintenance, and maintenance mode with
// its message: NovaWorldSignIn), then every session signed in now, the clients
// and the servers (NovaWorldSessions, #sessions), then the lobby instances
// holding them (NovaWorldLobbies, #lobbies). Refresh reads them all again.
import { computed, ref } from 'vue';
import { RouterLink } from 'vue-router';
import AdminPageHeader from '../../components/AdminPageHeader.vue';
import NovaWorldLobbies from '../../components/NovaWorldLobbies.vue';
import NovaWorldSessions from '../../components/NovaWorldSessions.vue';
import NovaWorldSignIn from '../../components/NovaWorldSignIn.vue';

const signIn = ref<InstanceType<typeof NovaWorldSignIn> | null>(null);
const sessions = ref<InstanceType<typeof NovaWorldSessions> | null>(null);
const lobbies = ref<InstanceType<typeof NovaWorldLobbies> | null>(null);
const refreshing = computed(() => !!signIn.value?.loading || !!sessions.value?.loading || !!lobbies.value?.loading);

function refresh() {
  void signIn.value?.refresh();
  void sessions.value?.refresh();
  void lobbies.value?.refresh();
}
</script>
