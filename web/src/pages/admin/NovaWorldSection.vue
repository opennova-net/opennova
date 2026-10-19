<template>
  <div class="space-y-10">
    <AdminPageHeader title="Status and sessions" :loading="refreshing" @refresh="refresh">
      <template #description>
        The service retail players sign in to: whether they can sign in now, and who is signed in. Its lobby
        instances are on <RouterLink to="/admin/infrastructure" class="text-accent hover:text-ink-bright">Infrastructure</RouterLink>,
        the Delta Force Xtreme 2 keys on <RouterLink to="/admin/cd-keys" class="text-accent hover:text-ink-bright">CD keys</RouterLink>.
      </template>
    </AdminPageHeader>

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
import AdminPageHeader from '../../components/AdminPageHeader.vue';
import NovaWorldSessions from '../../components/NovaWorldSessions.vue';
import NovaWorldSignIn from '../../components/NovaWorldSignIn.vue';

const signIn = ref<InstanceType<typeof NovaWorldSignIn> | null>(null);
const sessions = ref<InstanceType<typeof NovaWorldSessions> | null>(null);
const refreshing = computed(() => !!signIn.value?.loading || !!sessions.value?.loading);

function refresh() {
  void signIn.value?.refresh();
  void sessions.value?.refresh();
}
</script>
