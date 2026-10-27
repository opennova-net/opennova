<template>
  <div class="space-y-6">
    <PageHeader title="Players" :tabs="PLAYERS_TABS" />
  </div>
</template>

<script setup lang="ts">
// /admin/players: who is on NovaWorld now (a stub until its page lands).
import PageHeader from '../../../components/ui/PageHeader.vue';
import { PLAYERS_TABS } from '../../../utils/adminNav';
</script>
