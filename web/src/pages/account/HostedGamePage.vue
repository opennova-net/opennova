<template>
  <div class="space-y-6">
    <PageHeader title="Server" :crumbs="[{ label: 'Servers', to: '/me/servers' }]" />
  </div>
</template>

<script setup lang="ts">
// /me/servers/hosts/<rid>: a game the account hosts from the game itself (a
// stub until its page lands).
import PageHeader from '../../components/ui/PageHeader.vue';
</script>
