<template>
  <div class="space-y-6">
    <PageHeader title="Servers" :tabs="SERVERS_TABS" />
  </div>
</template>

<script setup lang="ts">
// /admin/servers/machines: the fleet's machines (a stub until its page lands).
import PageHeader from '../../../components/ui/PageHeader.vue';
import { SERVERS_TABS } from '../../../utils/adminNav';
</script>
