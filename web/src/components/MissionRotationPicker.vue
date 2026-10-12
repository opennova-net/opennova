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
      <template v-if="available.length > 0">
        Choose from the {{ available.length }} mission{{ available.length === 1 ? '' : 's' }} this service
        offers, up to {{ MISSIONS_MAX }}.
      </template>
      <template v-else>
        This service names no missions of its own, so add each map by its file name (letters, digits,
        _ and -, then .bms), up to {{ MISSIONS_MAX }}.
      </template>
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
        <ol v-else :aria-labelledby="`${baseId}-chosen`" class="divide-y divide-border rounded-panel border border-border">
          <li
            v-for="(mission, index) in modelValue"
            :key="mission"
            class="flex flex-wrap items-center gap-2 px-3 py-2 text-sm"
          >
            <span class="w-6 shrink-0 text-right font-mono text-xs text-ink-muted">{{ index + 1 }}.</span>
            <span class="min-w-0 flex-1 break-all font-mono text-ink">{{ mission }}</span>
            <span v-if="index === 0" class="rounded-control bg-accent/15 px-2 py-0.5 text-xs text-accent">Starts here</span>
            <span class="flex shrink-0 gap-1">
              <button
                :id="buttonId('up', mission)"
                type="button"
                :disabled="disabled || index === 0"
                :class="iconButton"
                @click="move(index, -1, 'up')"
              >
                <span aria-hidden="true">↑</span><span class="sr-only">Move {{ mission }} up</span>
              </button>
              <button
                :id="buttonId('down', mission)"
                type="button"
                :disabled="disabled || index === modelValue.length - 1"
                :class="iconButton"
                @click="move(index, 1, 'down')"
              >
                <span aria-hidden="true">↓</span><span class="sr-only">Move {{ mission }} down</span>
              </button>
              <button
                :id="buttonId('remove', mission)"
                type="button"
                :disabled="disabled"
                class="rounded-control border border-border px-2 py-1 text-xs text-danger/80 transition hover:text-danger disabled:opacity-40"
                @click="remove(index)"
              >
                Remove<span class="sr-only"> {{ mission }}</span>
              </button>
            </span>
          </li>
        </ol>
      </div>

      <div class="min-w-0 space-y-2">
        <template v-if="available.length > 0">
          <h3 :id="`${baseId}-available`" class="text-sm font-medium text-ink">Add a mission</h3>
          <p v-if="notChosen.length === 0" class="text-sm text-ink-muted">Every mission is in the rotation.</p>
          <ul v-else :aria-labelledby="`${baseId}-available`" class="divide-y divide-border rounded-panel border border-border">
            <li v-for="(mission, index) in notChosen" :key="mission" class="flex items-center gap-2 px-3 py-2 text-sm">
              <span class="min-w-0 flex-1 break-all font-mono text-ink">{{ mission }}</span>
              <button
                :id="buttonId('add', mission)"
                type="button"
                :disabled="disabled || full"
                class="rounded-control border border-border bg-raised px-2 py-1 text-xs text-ink transition hover:bg-hover disabled:opacity-40"
                @click="pick(mission, index)"
              >
                Add<span class="sr-only"> {{ mission }}</span>
              </button>
            </li>
          </ul>
          <button
            type="button"
            :disabled="disabled || sameAsDefault"
            class="text-xs text-accent hover:text-ink-bright disabled:opacity-40"
            @click="useDefault"
          >
            Use the service's rotation
          </button>
        </template>
        <template v-else>
          <label :for="`${baseId}-custom`" class="block text-sm font-medium text-ink">Add a mission by file name</label>
          <div class="flex gap-2">
            <input
              :id="`${baseId}-custom`"
              v-model="custom"
              type="text"
              autocomplete="off"
              autocapitalize="none"
              spellcheck="false"
              placeholder="mission.bms"
              :maxlength="MISSION_STEM_MAX + 4"
              :disabled="disabled"
              :aria-invalid="customError ? 'true' : undefined"
              :aria-describedby="customError ? `${baseId}-custom-error` : undefined"
              class="min-w-0 flex-1 rounded-control border border-border bg-surface px-3 py-2 font-mono text-sm text-ink focus:border-accent focus:outline-none"
              @keydown.enter.prevent="addCustom"
            />
            <button
              type="button"
              :disabled="disabled || full"
              class="rounded-control border border-border bg-raised px-4 py-2 text-sm font-medium text-ink transition hover:bg-hover disabled:opacity-40"
              @click="addCustom"
            >
              Add
            </button>
          </div>
          <p v-if="customError" :id="`${baseId}-custom-error`" class="text-xs text-danger">{{ customError }}</p>
        </template>
      </div>
    </div>
  </fieldset>
</template>

<script setup lang="ts">
// The rotation editor of the hosted server form: an ordered list of mission
// file names (the first the starting map), picked from the service's own list
// (GET /api/me/server default_missions) or, when it names none, typed by file
// name. Each mission is in the rotation at most once.
import { computed, nextTick, ref } from 'vue';
import { isMissionName, MISSION_STEM_MAX, MISSIONS_MAX } from '../utils/hostedSettings';

const props = defineProps<{
  modelValue: string[];
  // The service's missions; empty when it names none.
  available: string[];
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
const custom = ref('');
const customError = ref('');

const chosen = computed(() => new Set(props.modelValue.map((m) => m.toLowerCase())));
const notChosen = computed(() => props.available.filter((m) => !chosen.value.has(m.toLowerCase())));
const full = computed(() => props.modelValue.length >= MISSIONS_MAX);
const sameAsDefault = computed(
  () =>
    props.modelValue.length === props.available.length &&
    props.modelValue.every((m, i) => m === props.available[i]),
);

function buttonId(kind: 'up' | 'down' | 'remove' | 'add', mission: string): string {
  return `${props.baseId}-${kind}-${mission}`;
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
  update(next, `${mission} moved to position ${target + 1} of ${next.length}.`);
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
  update(next, `${mission} removed from the rotation.`);
  await nextTick();
  // The keyboard goes to the mission that took its place, else the legend.
  const neighbour = next[index] ?? next[index - 1];
  const el = neighbour ? document.getElementById(buttonId('remove', neighbour)) : null;
  if (el) el.focus();
  else legend.value?.focus();
}

function add(mission: string) {
  if (full.value || chosen.value.has(mission.toLowerCase())) return;
  update([...props.modelValue, mission], `${mission} added at position ${props.modelValue.length + 1}.`);
}

// An Add from the service's list: its row leaves that list, so the keyboard
// goes to the row that took its place, else the mission just added.
async function pick(mission: string, index: number) {
  const rest = notChosen.value.filter((m) => m !== mission);
  add(mission);
  await nextTick();
  const neighbour = rest[index] ?? rest[index - 1];
  const el = document.getElementById(neighbour ? buttonId('add', neighbour) : buttonId('remove', mission));
  if (el && !(el as HTMLButtonElement).disabled) el.focus();
  else legend.value?.focus();
}

function addCustom() {
  const name = custom.value.trim();
  if (name === '') {
    customError.value = 'Type a mission file name first.';
    return;
  }
  if (!isMissionName(name)) {
    customError.value = `That is not a mission file name: up to ${MISSION_STEM_MAX} letters, digits, _ and -, then .bms.`;
    return;
  }
  if (chosen.value.has(name.toLowerCase())) {
    customError.value = `${name} is in the rotation already.`;
    return;
  }
  if (full.value) {
    customError.value = `A rotation holds ${MISSIONS_MAX} missions at most.`;
    return;
  }
  customError.value = '';
  custom.value = '';
  add(name);
}

function useDefault() {
  update([...props.available], "The rotation is the service's own again.");
}

// The form's first-error focus lands on the legend.
function focus() {
  legend.value?.focus();
}

defineExpose({ focus });
</script>
