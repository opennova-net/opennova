<template>
  <ol class="slot-strip grid grid-cols-4 gap-1 sm:grid-cols-6" :aria-label="`Machine ${machineId}'s slots`">
    <li v-for="cell in cells" :key="cell.slot" class="slot-cell min-w-0">
      <RouterLink
        v-if="cell.holder"
        :to="`/admin/fleet/${cell.holder.server_id}`"
        class="block h-full rounded-control border px-1.5 py-1 text-xs transition hover:bg-hover"
        :class="cell.box"
        :title="`Slot ${cell.slot}: ${cell.name}, ${cell.word.toLowerCase()}`"
      >
        <span class="block font-mono text-ink-muted">{{ cell.slot }}</span>
        <span class="block truncate font-semibold">{{ cell.word }}</span>
        <span class="sr-only">: {{ cell.name }}</span>
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
// free one, or the server holding it (the machine's slot_holders), to its
// page, with its state in a word and a colour, "late" when its last minute had
// late ticks (PF1: it could not keep its 62.5 Hz). A server's name and ticks
// come from the fleet list where it holds the server.
import { computed } from 'vue';
import { RouterLink } from 'vue-router';
import type { FleetServer, SlotHolder } from '../types/hostedServer';
import { stateLabel } from '../utils/hostedState';

const props = withDefaults(
  defineProps<{
    machineId: string;
    slots: number;
    holders: readonly SlotHolder[];
    // The servers the page has read (their names and ticks).
    servers?: readonly FleetServer[];
  }>(),
  { servers: () => [] },
);

interface Cell {
  slot: number;
  holder: SlotHolder | null;
  name: string;
  word: string;
  box: string;
}

function cellOf(slot: number, holder: SlotHolder | null): Cell {
  if (!holder) return { slot, holder, name: '', word: 'Free', box: '' };
  const server = props.servers.find((s) => s.id === holder.server_id) ?? null;
  const name = server?.settings.game_name || `Server ${holder.server_id}`;
  const late = (server?.tick?.late ?? 0) > 0;
  if (holder.state === 'running') {
    return late
      ? { slot, holder, name, word: 'Running late', box: 'border-warn/40 bg-warn/10 text-warn' }
      : { slot, holder, name, word: 'Running', box: 'border-online/40 bg-online/10 text-online' };
  }
  if (holder.state === 'failed') return { slot, holder, name, word: 'Failed', box: 'border-danger/40 bg-danger/10 text-danger' };
  const live = holder.state !== 'stopped' && holder.state !== 'failed';
  const word = holder.state === 'stopping' ? 'Stopping' : live ? 'Starting' : stateLabel(holder.state);
  return { slot, holder, name, word, box: 'border-warn/40 bg-warn/10 text-warn' };
}

const cells = computed(() => {
  const bySlot = new Map<number, SlotHolder>();
  for (const h of props.holders) if (h.slot < props.slots) bySlot.set(h.slot, h);
  return Array.from({ length: props.slots }, (_, slot) => cellOf(slot, bySlot.get(slot) ?? null));
});
</script>
