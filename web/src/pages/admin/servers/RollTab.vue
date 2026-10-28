<template>
  <div class="space-y-6">
    <PageHeader title="Servers" :tabs="SERVERS_TABS" />
    <FleetRollPreview />
  </div>
</template>

<script setup lang="ts">
// /admin/servers/roll: moving the fleet's machines to a new image, from its
// preview.
import FleetRollPreview from '../../../components/FleetRollPreview.vue';
import PageHeader from '../../../components/ui/PageHeader.vue';
import { SERVERS_TABS } from '../../../utils/adminNav';
</script>
