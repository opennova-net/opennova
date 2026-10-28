<template>
  <div class="space-y-6">
    <PageHeader title="Servers" :tabs="MY_SERVERS_TABS" />
  </div>
</template>

<script setup lang="ts">
// /me/servers/stats: the account's servers together over time (a stub until
// its page lands).
import PageHeader from '../../components/ui/PageHeader.vue';
import { MY_SERVERS_TABS } from '../../utils/accountNav';
</script>
