<template>
  <span :class="['badge', tone === 'muted' ? '' : `badge-${tone}`]"><slot /></span>
</template>

<script setup lang="ts">
// A state word in a pill, coloured by its tone (utils/tone.ts maps each kind
// of state to one).
import type { Tone } from '../../utils/tone';

withDefaults(defineProps<{ tone?: Tone }>(), { tone: 'muted' });
</script>
