<template>
  <div class="space-y-8">
    <PageHeader title="System" :tabs="SYSTEM_TABS" refreshable :loading="refreshing" @refresh="refresh" />

    <NovaWorldSignIn ref="signIn" />
    <div id="lobbies" class="scroll-mt-20">
      <NovaWorldLobbies ref="lobbies" />
    </div>
  </div>
</template>

<script setup lang="ts">
// /admin/system: NovaWorld as retail players meet it, its sign-in and the
// lobby instances holding them. Who is online is Players' page, every listed
// server Servers'.
import { computed, ref } from 'vue';
import NovaWorldLobbies from '../../../components/NovaWorldLobbies.vue';
import NovaWorldSignIn from '../../../components/NovaWorldSignIn.vue';
import PageHeader from '../../../components/ui/PageHeader.vue';
import { SYSTEM_TABS } from '../../../utils/adminNav';

const signIn = ref<InstanceType<typeof NovaWorldSignIn> | null>(null);
const lobbies = ref<InstanceType<typeof NovaWorldLobbies> | null>(null);
const refreshing = computed(() => !!signIn.value?.loading || !!lobbies.value?.loading);

function refresh() {
  void signIn.value?.refresh();
  void lobbies.value?.refresh();
}
</script>
