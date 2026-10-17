<template>
  <fieldset
    class="min-w-0 space-y-3"
    :aria-describedby="error ? `${baseId}-hint ${baseId}-error` : `${baseId}-hint`"
  >
    <legend
      ref="legend"
      tabindex="-1"
      class="text-xs font-medium uppercase tracking-wider text-ink-muted focus:outline-none"
    >
      Mission rotation
    </legend>
    <p :id="`${baseId}-hint`" class="text-xs text-ink-muted">
      The server plays these maps in order and then starts over; the first is the map it starts on.
      Choose from the {{ offered.length }} mission{{ offered.length === 1 ? '' : 's' }} the game
      <template v-if="withExpansion">and its expansion hold</template><template v-else>holds</template>, up to
      {{ MISSIONS_MAX }}. A new server plays all of them unless you take some out.
    </p>
    <p v-if="error" :id="`${baseId}-error`" class="text-xs text-danger">{{ error }}</p>

    <!-- What a move or a removal did, for a screen reader. -->
    <p role="status" class="sr-only">{{ announcement }}</p>

    <div class="grid gap-4 md:grid-cols-2">
      <div class="min-w-0 space-y-2">
        <h3 :id="`${baseId}-chosen`" class="text-sm font-medium text-ink">
          In the rotation ({{ modelValue.length }})
        </h3>
        <p v-if="modelValue.length === 0" class="rounded-panel border border-dashed border-border-strong p-3 text-sm text-ink-muted">
          No missions yet.
        </p>
        <ol v-else :aria-labelledby="`${baseId}-chosen`" class="max-h-96 divide-y divide-border overflow-y-auto rounded-panel border border-border">
          <li
            v-for="(mission, index) in modelValue"
            :key="mission"
            class="flex flex-wrap items-center gap-2 px-3 py-2 text-sm"
          >
            <span class="w-6 shrink-0 text-right font-mono text-xs text-ink-muted">{{ index + 1 }}.</span>
            <span class="min-w-0 flex-1 break-words">
              <span class="block text-ink">{{ labelOf(mission) }}</span>
              <span v-if="labelOf(mission) !== mission" class="block break-all font-mono text-xs text-ink-muted">{{ mission }}</span>
            </span>
            <span v-if="index === 0" class="rounded-control bg-accent/15 px-2 py-0.5 text-xs text-accent">Starts here</span>
            <span class="flex shrink-0 gap-1">
              <button
                :id="buttonId('up', mission)"
                type="button"
                :disabled="disabled || index === 0"
                :class="iconButton"
                @click="move(index, -1, 'up')"
              >
                <span aria-hidden="true">↑</span><span class="sr-only">Move {{ labelOf(mission) }} up</span>
              </button>
              <button
                :id="buttonId('down', mission)"
                type="button"
                :disabled="disabled || index === modelValue.length - 1"
                :class="iconButton"
                @click="move(index, 1, 'down')"
              >
                <span aria-hidden="true">↓</span><span class="sr-only">Move {{ labelOf(mission) }} down</span>
              </button>
              <button
                :id="buttonId('remove', mission)"
                type="button"
                :disabled="disabled"
                class="rounded-control border border-border px-2 py-1 text-xs text-danger/80 transition hover:text-danger disabled:opacity-40"
                @click="remove(index)"
              >
                Remove<span class="sr-only"> {{ labelOf(mission) }}</span>
              </button>
            </span>
          </li>
        </ol>
      </div>

      <div class="min-w-0 space-y-2">
        <h3 :id="`${baseId}-available`" class="text-sm font-medium text-ink">Add a mission</h3>
        <p v-if="offered.length === 0" class="text-sm text-ink-muted">The game's data lists no missions.</p>
        <p v-else-if="notChosen.length === 0" class="text-sm text-ink-muted">Every mission is in the rotation.</p>
        <ul v-else :aria-labelledby="`${baseId}-available`" class="max-h-96 divide-y divide-border overflow-y-auto rounded-panel border border-border">
          <li v-for="(mission, index) in notChosen" :key="mission.file" class="flex items-center gap-2 px-3 py-2 text-sm">
            <span class="min-w-0 flex-1 break-words">
              <span class="block text-ink">{{ missionLabel(mission) }}</span>
              <span v-if="mission.title" class="block break-all font-mono text-xs text-ink-muted">{{ mission.file }}</span>
            </span>
            <button
              :id="buttonId('add', mission.file)"
              type="button"
              :disabled="disabled || full"
              class="rounded-control border border-border bg-raised px-2 py-1 text-xs text-ink transition hover:bg-hover disabled:opacity-40"
              @click="pick(mission.file, index)"
            >
              Add<span class="sr-only"> {{ missionLabel(mission) }}</span>
            </button>
          </li>
        </ul>
        <button
          v-if="offered.length > 0"
          type="button"
          :disabled="disabled || sameAsAll"
          class="text-xs text-accent hover:text-ink-bright disabled:opacity-40"
          @click="useAll"
        >
          Play every mission
        </button>
      </div>
    </div>
  </fieldset>
</template>

<script setup lang="ts">
// The rotation editor of the hosted server form: an ordered list of mission
// files (the first the starting map), picked from the missions the picked
// game's data holds (GET /api/me/servers, its data set's and expansion's
// missions), shown by title with the file beneath. Each mission is in the
// rotation at most once (the form dedupes the rotation it starts from), so a
// mission's file keys its row and its ids.
import { computed, nextTick, ref } from 'vue';
import type { HostedMission } from '../types/hostedServer';
import { MISSIONS_MAX, missionLabel } from '../utils/hostedSettings';

const props = defineProps<{
  modelValue: string[];
  // The missions the picked game and expansion hold, each file once.
  available: HostedMission[];
  // The form runs the game with an expansion (the hint says whose missions
  // these are).
  withExpansion?: boolean;
  // The prefix of every id inside.
  baseId: string;
  // The form's own error for the rotation.
  error?: string;
  disabled?: boolean;
}>();

const emit = defineEmits<{
  (e: 'update:modelValue', value: string[]): void;
}>();

const iconButton =
  'rounded-control border border-border px-2 py-1 text-xs text-ink transition hover:bg-hover disabled:opacity-30';

const legend = ref<HTMLElement | null>(null);
const announcement = ref('');

const offered = computed(() => props.available);
const titles = computed(() => new Map(offered.value.map((m) => [m.file.toLowerCase(), missionLabel(m)] as const)));
const chosen = computed(() => new Set(props.modelValue.map((m) => m.toLowerCase())));
const notChosen = computed(() => offered.value.filter((m) => !chosen.value.has(m.file.toLowerCase())));
const full = computed(() => props.modelValue.length >= MISSIONS_MAX);
const sameAsAll = computed(
  () =>
    props.modelValue.length === offered.value.length &&
    props.modelValue.every((m, i) => m.toLowerCase() === offered.value[i].file.toLowerCase()),
);

// A mission's title, or its file when the data names none (or the file is
// no longer offered).
function labelOf(file: string): string {
  return titles.value.get(file.toLowerCase()) ?? file;
}

function buttonId(kind: 'up' | 'down' | 'remove' | 'add', file: string): string {
  return `${props.baseId}-${kind}-${file}`;
}

function update(next: string[], said: string) {
  emit('update:modelValue', next);
  announcement.value = said;
}

async function move(index: number, by: -1 | 1, kind: 'up' | 'down') {
  const target = index + by;
  if (target < 0 || target >= props.modelValue.length) return;
  const next = [...props.modelValue];
  const [mission] = next.splice(index, 1);
  next.splice(target, 0, mission);
  update(next, `${labelOf(mission)} moved to position ${target + 1} of ${next.length}.`);
  // The keyboard stays on the button that moved it, unless it can move no
  // further that way (then on the other).
  await nextTick();
  const atEdge = kind === 'up' ? target === 0 : target === next.length - 1;
  const id = buttonId(atEdge ? (kind === 'up' ? 'down' : 'up') : kind, mission);
  document.getElementById(id)?.focus();
}

async function remove(index: number) {
  const next = [...props.modelValue];
  const [mission] = next.splice(index, 1);
  update(next, `${labelOf(mission)} removed from the rotation.`);
  await nextTick();
  // The keyboard goes to the mission that took its place, else the legend.
  const neighbour = next[index] ?? next[index - 1];
  const el = neighbour ? document.getElementById(buttonId('remove', neighbour)) : null;
  if (el) el.focus();
  else legend.value?.focus();
}

function add(file: string) {
  if (full.value || chosen.value.has(file.toLowerCase())) return;
  update([...props.modelValue, file], `${labelOf(file)} added at position ${props.modelValue.length + 1}.`);
}

// An Add from the list: its row leaves that list, so the keyboard goes to
// the row that took its place, else the mission just added.
async function pick(file: string, index: number) {
  const rest = notChosen.value.filter((m) => m.file !== file);
  add(file);
  await nextTick();
  const neighbour = rest[index] ?? rest[index - 1];
  const el = document.getElementById(neighbour ? buttonId('add', neighbour.file) : buttonId('remove', file));
  if (el && !(el as HTMLButtonElement).disabled) el.focus();
  else legend.value?.focus();
}

function useAll() {
  update(
    offered.value.map((m) => m.file),
    'The rotation holds every mission again.',
  );
}

// The form's first-error focus lands on the legend.
function focus() {
  legend.value?.focus();
}

defineExpose({ focus });
</script>
