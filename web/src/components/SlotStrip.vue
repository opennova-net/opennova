<template>
  <ol class="slot-strip grid grid-cols-2 gap-1 sm:grid-cols-3" :aria-label="`Machine ${machineId}'s slots`">
    <li v-for="cell in cells" :key="cell.slot" class="slot-cell min-w-0">
      <RouterLink
        v-if="cell.holder"
        :to="`/admin/servers/${cell.holder.server_id}`"
        class="block h-full min-w-0 rounded-control border px-2 py-1 text-xs transition hover:bg-hover"
        :class="BOX[cell.tone]"
        :title="`Slot ${cell.slot}: ${cell.word}`"
      >
        <span class="flex min-w-0 items-baseline gap-1.5">
          <span class="shrink-0 font-mono opacity-70">{{ cell.slot }}</span>
          <GameText :text="serverName(cell.name)" class="min-w-0 truncate font-semibold text-ink" />
        </span>
        <span class="block truncate">
          {{ [cell.game, cell.players, cell.word === 'Running' ? '' : cell.word].filter(Boolean).join(' · ') }}
        </span>
        <span class="sr-only">{{ cell.word }}</span>
      </RouterLink>
      <span v-else class="block h-full rounded-control border border-dashed border-border px-2 py-1 text-xs text-ink-muted">
        <span class="font-mono">{{ cell.slot }}</span> Free
      </span>
    </li>
  </ol>
</template>

<script setup lang="ts">
// A machine's slots, a cell each: free, or the server in it, to its page, by
// name, game and players, its state in the cell's colour (utils/machineView.ts
// slotCell).
import { computed } from 'vue';
import { RouterLink } from 'vue-router';
import GameText from './GameText.vue';
import type { FleetServer, SlotHolder } from '../types/hostedServer';
import { serverName } from '../utils/games';
import { slotCell } from '../utils/machineView';
import type { Tone } from '../utils/tone';

const props = withDefaults(
  defineProps<{
    machineId: string;
    slots: number;
    holders: readonly SlotHolder[];
    // The servers the page has read, to name the holders that name none.
    servers?: readonly FleetServer[];
  }>(),
  { servers: () => [] },
);

const BOX: Record<Tone, string> = {
  ok: 'border-online/40 bg-online/10 text-online',
  warn: 'border-warn/40 bg-warn/10 text-warn',
  bad: 'border-danger/40 bg-danger/10 text-danger',
  info: 'border-accent/40 bg-accent/10 text-accent',
  muted: 'border-border text-ink-muted',
};

const cells = computed(() => {
  const bySlot = new Map<number, SlotHolder>();
  for (const h of props.holders) if (h.slot < props.slots) bySlot.set(h.slot, h);
  return Array.from({ length: props.slots }, (_, slot) => slotCell(slot, bySlot.get(slot) ?? null, props.servers));
});
</script>
