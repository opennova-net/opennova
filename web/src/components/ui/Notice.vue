<template>
  <div :role="role" :class="['notice', toneClass]"><slot /></div>
</template>

<script setup lang="ts">
// A boxed message in a tone: ok (done), warn, bad (an error), info (a pointer
// worth acting on) or neutral. `role` makes a screen reader say it.
import { computed } from 'vue';

const props = withDefaults(
  defineProps<{
    tone?: 'ok' | 'warn' | 'bad' | 'info' | 'neutral';
    role?: 'status' | 'alert';
  }>(),
  { tone: 'neutral', role: undefined },
);

const toneClass = computed(() => (props.tone === 'neutral' ? '' : `notice-${props.tone}`));
</script>
