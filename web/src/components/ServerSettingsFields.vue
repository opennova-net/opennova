<template>
  <div class="space-y-6">
    <div class="flex flex-wrap items-end justify-between gap-3">
      <label :for="`${baseId}-search`" class="min-w-0 flex-1 sm:max-w-sm">
        <span class="field-label">Find a setting</span>
        <input :id="`${baseId}-search`" v-model="search" type="search" class="field" placeholder="Teams, ping, weapons…" />
      </label>
      <p v-if="live" class="text-xs text-ink-muted">
        Settings tagged <span :class="TAG_CLASS">Next launch</span> take effect when the server starts again; the rest apply now.
      </p>
    </div>

    <p v-if="sections.length === 0" class="text-sm text-ink-muted">No setting matches “{{ search }}”.</p>

    <section v-for="section in sections" :key="section.group" class="space-y-3" :aria-labelledby="`${baseId}-group-${slug(section.group)}`">
      <h3 :id="`${baseId}-group-${slug(section.group)}`" class="border-b border-border pb-1 text-sm font-semibold text-ink-bright">{{ section.group }}</h3>

      <!-- The plain switches, as a checklist. -->
      <div v-if="section.switches.length" class="grid gap-x-6 gap-y-2 sm:grid-cols-2 lg:grid-cols-3">
        <label v-for="spec in section.switches" :key="spec.key" class="flex min-w-0 items-start gap-2 text-sm text-ink" :title="spec.note || undefined">
          <input :id="fieldId(spec)" type="checkbox" class="mt-0.5 h-4 w-4 shrink-0 accent-accent" :checked="valueOf(spec) === true" :disabled="disabled"
            @change="set(spec.key, ($event.target as HTMLInputElement).checked)" />
          <span class="min-w-0">{{ spec.label }} <span v-if="live && !spec.command" :class="TAG_CLASS">Next launch</span></span>
        </label>
      </div>

      <!-- The values, each a setting with the ones that depend on it beneath. -->
      <div v-if="section.units.length" class="grid gap-x-6 gap-y-4 sm:grid-cols-2 lg:grid-cols-3">
        <div v-for="unit in section.units" :key="unit.parent.key" class="min-w-0 space-y-3">
          <div v-for="spec in [unit.parent, ...unit.children]" :key="spec.key" class="min-w-0"
            :class="spec === unit.parent ? '' : 'border-l-2 border-border pl-3'">
            <label v-if="spec.kind === 'boolean'" class="flex items-start gap-2 text-sm text-ink" :title="spec.note || undefined">
              <input :id="fieldId(spec)" type="checkbox" class="mt-0.5 h-4 w-4 shrink-0 accent-accent" :checked="valueOf(spec) === true"
                :disabled="disabled || !parentOn(spec)" @change="set(spec.key, ($event.target as HTMLInputElement).checked)" />
              <span>{{ spec.label }} <span v-if="live && !spec.command" :class="TAG_CLASS">Next launch</span></span>
            </label>
            <template v-else>
              <label :for="fieldId(spec)" class="field-label">
                {{ spec.label }} <span v-if="live && !spec.command" :class="TAG_CLASS">Next launch</span>
              </label>
              <div class="flex items-center gap-2">
                <input :id="fieldId(spec)" :type="inputType(spec)" :value="valueOf(spec)"
                  :min="isNumeric(spec) ? spec.min : undefined" :max="isNumeric(spec) ? spec.max : undefined"
                  :step="spec.kind === 'number' ? 'any' : 1" :maxlength="isNumeric(spec) ? undefined : spec.max"
                  :disabled="disabled || !parentOn(spec)" class="field" :class="widthOf(spec)"
                  autocomplete="off" spellcheck="false" :aria-invalid="problem(spec) ? true : undefined" :aria-describedby="`${fieldId(spec)}-hint`"
                  @input="input(spec, $event)" />
                <button v-if="spec.kind === 'password'" type="button" class="btn-secondary btn-sm" :aria-pressed="shown.has(spec.key) ? 'true' : 'false'"
                  :aria-controls="fieldId(spec)" @click="toggleShown(spec.key)">
                  {{ shown.has(spec.key) ? 'Hide' : 'Show' }}<span class="sr-only"> {{ spec.label }}</span>
                </button>
                <span v-if="isNumeric(spec) && rangeOf(spec)" class="text-xs text-ink-muted">{{ rangeOf(spec) }}</span>
              </div>
            </template>
            <p :id="`${fieldId(spec)}-hint`" class="mt-1 text-xs" :class="problem(spec) ? 'text-danger' : 'text-ink-muted'">
              {{ problem(spec) || hintOf(spec) }}
            </p>
          </div>
        </div>
      </div>

      <!-- Classes and weapons: the class switches and the weapon roster. -->
      <template v-if="section.group === WEAPONS_GROUP">
        <div v-if="showClasses" class="space-y-2">
          <p class="field-label">Classes players may pick <span v-if="live" :class="TAG_CLASS">Next launch</span></p>
          <div class="flex flex-wrap gap-x-6 gap-y-2">
            <label v-for="name in classes" :key="name" class="flex items-center gap-2 text-sm capitalize text-ink">
              <input type="checkbox" class="h-4 w-4 accent-accent" :checked="nested('classes')[name] !== false" :disabled="disabled"
                @change="setNested('classes', name, ($event.target as HTMLInputElement).checked)" />{{ name }}
            </label>
          </div>
        </div>
        <div v-if="shownWeapons.length" class="space-y-2">
          <div class="flex flex-wrap items-end justify-between gap-2">
            <p class="field-label mb-0">Weapons</p>
            <label class="flex items-center gap-2 text-xs text-ink-muted">Set all
              <select class="field w-auto py-1 text-xs" :disabled="disabled" value=""
                @change="setAllWeapons(($event.target as HTMLSelectElement).value); ($event.target as HTMLSelectElement).value = ''">
                <option value="" disabled>Choose…</option>
                <option v-for="option in AVAILABILITY" :key="option.value" :value="option.value">{{ option.label }}</option>
              </select>
            </label>
          </div>
          <div class="grid gap-x-6 gap-y-1.5 sm:grid-cols-2 lg:grid-cols-3">
            <label v-for="weapon in shownWeapons" :key="weapon.name" class="flex min-w-0 items-center justify-between gap-2 text-sm text-ink">
              <span class="min-w-0 truncate" :title="weapon.name">{{ weapon.name }}</span>
              <select class="field w-36 shrink-0 py-1 text-xs" :disabled="disabled" :value="nested('weapons')[weapon.name] ?? 'mission'"
                @change="setNested('weapons', weapon.name, ($event.target as HTMLSelectElement).value)">
                <option v-for="option in AVAILABILITY" :key="option.value" :value="option.value">{{ option.label }}</option>
              </select>
            </label>
          </div>
        </div>
        <p v-else-if="selectableWeapons.length === 0" class="text-xs text-ink-muted">This game has no weapon catalog available.</p>
      </template>
    </section>
  </div>
</template>

<script setup lang="ts">
// A server's game.cfg options (the shared schema, engine/formats/gamecfg/server_settings.cpp),
// by group: the plain switches as a checklist, then the values, a setting that does
// nothing without another (`depends_on`) beneath that one and disabled while it is off.
// The "Classes and weapons" group ends with the class switches and the weapon roster.
// On a live server (`live`) a setting with no SET verb is tagged for the next launch.
import { computed, ref } from 'vue';
import { settingProblem, type ServerSetting, type ServerOptions, type ServerWeapon, type SettingValue } from '../utils/serverConfiguration';

const props = withDefaults(
  defineProps<{ modelValue: ServerOptions; schema: ServerSetting[]; weapons?: ServerWeapon[]; disabled?: boolean; live?: boolean; baseId: string }>(),
  { weapons: () => [] },
);
const emit = defineEmits<{ (e: 'update:modelValue', value: ServerOptions): void }>();

const WEAPONS_GROUP = 'Classes and weapons';
const GROUP_ORDER = ['Server', 'Players and access', 'Teams', 'Rules', 'Rotation', WEAPONS_GROUP, 'Network'];
// A weapon's availability as the host screen's WEAPON_LIST cycles it: the title's MISSION
// word (the mission's own item availability decides; every weapon's default), always (1),
// the armory-only word, never (0) (engine/formats/gamecfg/server_settings.cpp).
const AVAILABILITY = [
  { value: 'mission', label: 'Mission default' },
  { value: 'always', label: 'Always available' },
  { value: 'armory', label: 'Armory only' },
  { value: 'never', label: 'Unavailable' },
];
// The quiet tag a live server's setting carries when it waits for the next launch.
const TAG_CLASS = 'ml-1 rounded bg-hover px-1.5 py-px text-[11px] font-normal text-ink-muted';
const INT32_MIN = -2147483648;
const INT32_MAX = 2147483647;
const classes = ['medic', 'sniper', 'gunner', 'rifleman', 'engineer'];

const search = ref('');
const shown = ref(new Set<string>());
const needle = computed(() => search.value.trim().toLowerCase());
const matches = (text: string) => !needle.value || text.toLowerCase().includes(needle.value);
const matchesSpec = (s: ServerSetting) => matches(`${s.label} ${s.group} ${s.key}`);

const byKey = computed(() => new Map(props.schema.map((s) => [s.key, s])));
const selectableWeapons = computed(() => props.weapons.filter((w) => w.selectable));
const shownWeapons = computed(() =>
  matches(WEAPONS_GROUP) || matches('weapons') ? selectableWeapons.value : selectableWeapons.value.filter((w) => matches(w.name)),
);
const showClasses = computed(() => matches(WEAPONS_GROUP) || matches('classes') || classes.some(matches));

interface Unit { parent: ServerSetting; children: ServerSetting[] }
interface Section { group: string; switches: ServerSetting[]; units: Unit[] }

// Each group's switches and units, those the search finds (a unit whole when any of
// it matches), in GROUP_ORDER and the schema's own order within a group.
const sections = computed<Section[]>(() => {
  const children = new Map<string, ServerSetting[]>();
  for (const s of props.schema) {
    if (s.depends_on && byKey.value.has(s.depends_on)) children.set(s.depends_on, [...(children.get(s.depends_on) ?? []), s]);
  }
  const out = new Map<string, Section>();
  for (const s of props.schema) {
    if (s.depends_on && byKey.value.has(s.depends_on)) continue;
    const kids = children.get(s.key) ?? [];
    if (![s, ...kids].some(matchesSpec)) continue;
    const section = out.get(s.group) ?? { group: s.group, switches: [], units: [] };
    if (s.kind === 'boolean' && kids.length === 0) section.switches.push(s);
    else section.units.push({ parent: s, children: kids });
    out.set(s.group, section);
  }
  if (!out.has(WEAPONS_GROUP) && (showClasses.value || shownWeapons.value.length > 0)) {
    out.set(WEAPONS_GROUP, { group: WEAPONS_GROUP, switches: [], units: [] });
  }
  const rank = (g: string) => {
    const i = GROUP_ORDER.indexOf(g);
    return i < 0 ? GROUP_ORDER.length : i;
  };
  return [...out.values()].sort((a, b) => rank(a.group) - rank(b.group));
});

const slug = (text: string) => text.toLowerCase().replace(/[^a-z0-9]+/g, '-');
const fieldId = (s: ServerSetting) => `${props.baseId}-${s.key}`;
const isNumeric = (s: ServerSetting) => s.kind === 'integer' || s.kind === 'number';
const inputType = (s: ServerSetting) => (isNumeric(s) ? 'number' : s.kind === 'password' && !shown.value.has(s.key) ? 'password' : 'text');
const valueOf = (s: ServerSetting) => props.modelValue[s.key] ?? s.default;
const problem = (s: ServerSetting) => settingProblem(s, valueOf(s));

// A dependent setting is live while the one it depends on is on (or nonzero).
function parentOn(s: ServerSetting): boolean {
  const parent = s.depends_on ? byKey.value.get(s.depends_on) : undefined;
  if (!parent) return true;
  const value = valueOf(parent);
  return value === true || (typeof value === 'number' && value !== 0);
}

// The range a value takes, when the schema bounds it more tightly than a 32-bit word.
function rangeOf(s: ServerSetting): string {
  const low = s.min > INT32_MIN, high = s.max < INT32_MAX;
  if (low && high) return `${s.min} to ${s.max}`;
  return '';
}

// A field's width: a number's, or the text's by the characters it takes.
function widthOf(s: ServerSetting): string {
  if (isNumeric(s)) return 'w-28';
  return s.max <= 8 ? 'w-28' : s.max <= 31 ? 'min-w-0 flex-1 sm:max-w-xs' : 'min-w-0 flex-1';
}

function hintOf(s: ServerSetting): string {
  if (!parentOn(s)) {
    const parent = byKey.value.get(s.depends_on)?.label.replace(/\s*\(.*\)$/, '') ?? s.depends_on;
    return `Used only with “${parent}” on.`;
  }
  if (s.kind === 'text' || s.kind === 'password') return s.note || `Up to ${s.max} characters.`;
  return s.note;
}

function toggleShown(key: string) {
  const next = new Set(shown.value);
  if (!next.delete(key)) next.add(key);
  shown.value = next;
}

function nested(key: string): Record<string, string | boolean> {
  const value = props.modelValue[key];
  return value && typeof value === 'object' ? value : {};
}
function set(key: string, value: SettingValue) {
  emit('update:modelValue', { ...props.modelValue, [key]: value });
}
function setNested(key: string, name: string, value: string | boolean) {
  emit('update:modelValue', { ...props.modelValue, [key]: { ...nested(key), [name]: value } });
}
function setAllWeapons(value: string) {
  emit('update:modelValue', { ...props.modelValue, weapons: Object.fromEntries(selectableWeapons.value.map((weapon) => [weapon.name, value])) });
}
function input(spec: ServerSetting, event: Event) {
  const text = (event.target as HTMLInputElement).value;
  set(spec.key, isNumeric(spec) ? (text === '' ? '' : Number(text)) : text);
}
</script>
