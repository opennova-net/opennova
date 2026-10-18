<template>
  <ol class="slot-strip grid grid-cols-4 gap-1 sm:grid-cols-6" :aria-label="`Machine ${machineId}'s slots`">
    <li v-for="cell in cells" :key="cell.slot" class="slot-cell min-w-0">
      <RouterLink
        v-if="cell.server"
        :to="`/admin/fleet/${cell.server.id}`"
        class="block h-full rounded-control border px-1.5 py-1 text-xs transition hover:bg-hover"
        :class="cell.box"
        :title="`Slot ${cell.slot}: ${cell.server.settings.game_name || 'Unnamed server'}, ${cell.word.toLowerCase()}`"
      >
        <span class="block font-mono text-ink-muted">{{ cell.slot }}</span>
        <span class="block truncate font-semibold">{{ cell.word }}</span>
        <span class="sr-only">: {{ cell.server.settings.game_name || 'Unnamed server' }}</span>
      </RouterLink>
      <span v-else class="block h-full rounded-control border border-dashed border-border px-1.5 py-1 text-xs text-ink-muted">
        <span class="block font-mono">{{ cell.slot }}</span>
        <span class="block">Free</span>
      </span>
    </li>
  </ol>
</template>

<script setup lang="ts">
// A machine's slots in a row, a cell each (slot k answers UDP 32768+k): a
// free one, or the server holding it, to its page, with its state in a word
// and a colour, "late" when its last minute had late ticks (PF1: it could not
// keep its 62.5 Hz).
import { computed } from 'vue';
import { RouterLink } from 'vue-router';
import type { FleetServer } from '../types/hostedServer';
import { stateLabel } from '../utils/hostedState';

const props = defineProps<{
  machineId: string;
  slots: number;
  // The live servers on the machine.
  servers: readonly FleetServer[];
}>();

interface Cell {
  slot: number;
  server: FleetServer | null;
  word: string;
  box: string;
}

function cellOf(slot: number, server: FleetServer | null): Cell {
  if (!server) return { slot, server, word: 'Free', box: '' };
  const late = (server.tick?.late ?? 0) > 0;
  if (server.state === 'running') {
    return late
      ? { slot, server, word: 'Running late', box: 'border-warn/40 bg-warn/10 text-warn' }
      : { slot, server, word: 'Running', box: 'border-online/40 bg-online/10 text-online' };
  }
  if (server.state === 'failed') return { slot, server, word: 'Failed', box: 'border-danger/40 bg-danger/10 text-danger' };
  const word = server.state === 'stopping' ? 'Stopping' : server.live ? 'Starting' : stateLabel(server.state);
  return { slot, server, word, box: 'border-warn/40 bg-warn/10 text-warn' };
}

const cells = computed(() => {
  const bySlot = new Map<number, FleetServer>();
  for (const s of props.servers) if (s.slot !== null) bySlot.set(s.slot, s);
  return Array.from({ length: props.slots }, (_, slot) => cellOf(slot, bySlot.get(slot) ?? null));
});
</script>
