<template>
  <span><span v-for="(run, i) in runs" :key="i" :style="runStyle(run)">{{ run.text }}</span></span>
</template>

<script setup lang="ts">
// A text in the game's inline markup (a server name, a mission, a message),
// drawn as the game draws it: its colours, bold, italic and underline, its tags
// left out (utils/gameText.ts). Each run is a text node in a styled span, so a
// name's markup can never become HTML.
import { computed } from 'vue';
import { parseGameText, type GameTextRun } from '../utils/gameText';

const props = defineProps<{
  text?: string | null;
}>();

const runs = computed(() => parseGameText(props.text));

// The half-bright of the original colour (<CH>) is three quarters of each
// channel plus a fixed bias [orig: 3 * (((c >> 2) & 0x3F3F3F) + 0x156B40)
// @0x674321..0x674336], of a colour the page does not know (it inherits the
// page's), so it shows as that colour dimmed.
const HALF_BRIGHT_OPACITY = '0.75';

function runStyle(run: GameTextRun): Record<string, string> {
  const style: Record<string, string> = {};
  if (run.color === 'half') style.opacity = HALF_BRIGHT_OPACITY;
  else if (run.color !== 'original') style.color = run.color;
  if (run.bold) style.fontWeight = '700';
  if (run.italic) style.fontStyle = 'italic';
  if (run.underline) style.textDecoration = 'underline';
  return style;
}
</script>
