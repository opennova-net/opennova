<template>
  <!-- Always in the page, so a screen reader hears what lands in them; each
       is a box only while it holds something. -->
  <div role="status" :class="message ? 'notice notice-ok' : 'sr-only'">{{ message }}</div>
  <div role="alert" :class="error ? 'notice notice-bad' : 'sr-only'">{{ error }}</div>
  <p v-if="pollTrouble" class="text-xs text-warn">Last refresh failed; retrying.</p>
</template>

<script setup lang="ts">
// A page's live messages: what an action did (`message`), what went wrong
// (`error`), and whether its last background read failed (`pollTrouble`).
withDefaults(defineProps<{ message?: string; error?: string; pollTrouble?: boolean }>(), {
  message: '',
  error: '',
  pollTrouble: false,
});
</script>
