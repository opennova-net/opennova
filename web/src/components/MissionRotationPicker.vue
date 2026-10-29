<template>
  <fieldset class="min-w-0 space-y-3" :aria-describedby="error ? `${baseId}-error` : undefined">
    <legend ref="legend" tabindex="-1" class="field-label focus:outline-none" :title="`Played in order, then again; up to ${MISSIONS_MAX} maps`">
      Mission rotation
    </legend>
    <p v-if="error" :id="`${baseId}-error`" class="text-xs text-danger">{{ error }}</p>
    <p class="text-xs text-ink-muted">Maps may repeat. Side switching applies to every occurrence of the same map.</p>

    <!-- What a move or a removal did, for a screen reader. -->
    <p role="status" class="sr-only">{{ announcement }}</p>

    <div class="grid gap-4 md:grid-cols-2">
      <div class="min-w-0 space-y-2">
        <h3 :id="`${baseId}-chosen`" class="text-sm font-medium text-ink">
          In the rotation ({{ modelValue.length }})
        </h3>
        <p v-if="modelValue.length === 0" class="card p-3 text-sm text-ink-muted">No missions yet.</p>
        <ol v-else :aria-labelledby="`${baseId}-chosen`" class="card max-h-96 divide-y divide-border overflow-y-auto">
          <li
            v-for="(mission, index) in modelValue"
            :key="index"
            class="flex flex-wrap items-center gap-2 px-3 py-2 text-sm"
          >
            <span class="w-6 shrink-0 text-right font-mono text-xs text-ink-muted">{{ index + 1 }}.</span>
            <span class="min-w-0 flex-1 break-words">
              <span class="block text-ink">{{ labelOf(mission) }}</span>
              <span v-if="labelOf(mission) !== mission" class="block break-all font-mono text-xs text-ink-muted">{{ mission }}</span>
            </span>
            <Badge v-if="index === 0" tone="info">Starts here</Badge>
            <label class="flex items-center gap-1 text-xs">
              <input type="checkbox" :checked="swapSides?.[index] === true" :disabled="disabled" @change="swap(index, ($event.target as HTMLInputElement).checked)" /> Swap at half
            </label>
            <span class="flex shrink-0 gap-1">
              <button
                :id="buttonId('up', String(index))"
                type="button"
                :disabled="disabled || index === 0"
                class="btn-ghost btn-sm"
                @click="move(index, -1, 'up')"
              >
                <span aria-hidden="true">↑</span><span class="sr-only">Move {{ labelOf(mission) }} up</span>
              </button>
              <button
                :id="buttonId('down', String(index))"
                type="button"
                :disabled="disabled || index === modelValue.length - 1"
                class="btn-ghost btn-sm"
                @click="move(index, 1, 'down')"
              >
                <span aria-hidden="true">↓</span><span class="sr-only">Move {{ labelOf(mission) }} down</span>
              </button>
              <button
                :id="buttonId('remove', String(index))"
                type="button"
                :disabled="disabled"
                class="btn-ghost btn-sm"
                @click="remove(index)"
              >
                Remove<span class="sr-only">{{ ' ' + (labelOf(mission)) }}</span>
              </button>
            </span>
          </li>
        </ol>
      </div>

      <div class="min-w-0 space-y-2">
        <h3 :id="`${baseId}-available`" class="text-sm font-medium text-ink">Add a mission</h3>
        <p v-if="offered.length === 0" class="text-sm text-ink-muted">No missions in this game's data.</p>
        <p v-else-if="notChosen.length === 0" class="text-sm text-ink-muted">All in the rotation.</p>
        <ul v-else :aria-labelledby="`${baseId}-available`" class="card max-h-96 divide-y divide-border overflow-y-auto">
          <li v-for="(mission, index) in notChosen" :key="mission.file" class="flex items-center gap-2 px-3 py-2 text-sm">
            <span class="min-w-0 flex-1 break-words">
              <span class="block text-ink">{{ missionLabel(mission) }}</span>
              <span v-if="mission.title" class="block break-all font-mono text-xs text-ink-muted">{{ mission.file }}</span>
            </span>
            <button
              :id="buttonId('add', mission.file)"
              type="button"
              :disabled="disabled || full"
              class="btn-ghost btn-sm"
              @click="pick(mission.file, index)"
            >
              Add<span class="sr-only">{{ ' ' + (missionLabel(mission)) }}</span>
            </button>
          </li>
        </ul>
        <button
          v-if="offered.length > 0"
          type="button"
          :disabled="disabled || sameAsAll"
          class="btn-ghost btn-sm"
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
// game's data holds (GET /api/me/servers, its game's and expansion's
// missions), shown by title with the file beneath. Entries may repeat, so
// rotation indexes identify their controls; side switching is shared by map.
import { computed, nextTick, ref } from 'vue';
import Badge from './ui/Badge.vue';
import type { HostedMission } from '../types/hostedServer';
import { MISSIONS_MAX, missionLabel } from '../utils/hostedSettings';

const props = defineProps<{
  modelValue: string[];
  swapSides?: boolean[];
  // The missions the picked game and expansion hold, each file once.
  available: HostedMission[];
  // The prefix of every id inside.
  baseId: string;
  // The form's own error for the rotation.
  error?: string;
  disabled?: boolean;
}>();

const emit = defineEmits<{
  (e: 'update:modelValue', value: string[]): void;
  (e: 'update:swapSides', value: boolean[]): void;
}>();

const legend = ref<HTMLElement | null>(null);
const announcement = ref('');

const offered = computed(() => props.available);
const titles = computed(() => new Map(offered.value.map((m) => [m.file.toLowerCase(), missionLabel(m)] as const)));
const notChosen = computed(() => offered.value);
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
  const flags = props.modelValue.map((_, i) => props.swapSides?.[i] === true);
  const [flag] = flags.splice(index, 1); flags.splice(target, 0, flag);
  emit('update:swapSides', flags);
  update(next, `${labelOf(mission)} moved to position ${target + 1} of ${next.length}.`);
  // The keyboard stays on the button that moved it, unless it can move no
  // further that way (then on the other).
  await nextTick();
  const atEdge = kind === 'up' ? target === 0 : target === next.length - 1;
  const id = buttonId(atEdge ? (kind === 'up' ? 'down' : 'up') : kind, String(target));
  document.getElementById(id)?.focus();
}

async function remove(index: number) {
  const next = [...props.modelValue];
  const [mission] = next.splice(index, 1);
  const flags = props.modelValue.map((_, i) => props.swapSides?.[i] === true); flags.splice(index, 1);
  emit('update:swapSides', flags);
  update(next, `${labelOf(mission)} removed from the rotation.`);
  await nextTick();
  // The keyboard goes to the mission that took its place, else the legend.
  const neighbour = next[index] ?? next[index - 1];
  const el = neighbour ? document.getElementById(buttonId('remove', String(Math.min(index, next.length - 1)))) : null;
  if (el) el.focus();
  else legend.value?.focus();
}

function add(file: string) {
  if (full.value) return;
  const existing = props.modelValue.findIndex((entry) => entry.toLowerCase() === file.toLowerCase());
  emit('update:swapSides', [...props.modelValue.map((_, i) => props.swapSides?.[i] === true), existing >= 0 && props.swapSides?.[existing] === true]);
  update([...props.modelValue, file], `${labelOf(file)} added at position ${props.modelValue.length + 1}.`);
}

// Keep keyboard focus on Add so a map can be repeated.
async function pick(file: string, index: number) {
  add(file);
  await nextTick();
  const el = document.getElementById(buttonId('add', file));
  if (el && !(el as HTMLButtonElement).disabled) el.focus();
  else legend.value?.focus();
}

function useAll() {
  emit('update:swapSides', offered.value.map(() => false));
  update(
    offered.value.map((m) => m.file),
    'The rotation holds every mission again.',
  );
}

function swap(index: number, enabled: boolean) {
  const flags = props.modelValue.map((_, i) => props.swapSides?.[i] === true);
  props.modelValue.forEach((file, i) => { if (file.toLowerCase() === props.modelValue[index]?.toLowerCase()) flags[i] = enabled; });
  emit('update:swapSides', flags);
}

// The form's first-error focus lands on the legend.
function focus() {
  legend.value?.focus();
}

defineExpose({ focus });
</script>
