<template>
  <div class="space-y-4">
    <label class="block text-sm">Find a setting
      <input v-model="search" type="search" class="field mt-1" placeholder="Search settings, teams, weapons…" />
    </label>
    <fieldset v-for="group in groups" :key="group" class="rounded-panel border border-border p-4">
      <legend class="px-2 font-medium text-ink">{{ group }}</legend>
      <div class="grid gap-4 sm:grid-cols-2 lg:grid-cols-3">
        <div v-for="spec in fields.filter((s) => s.group === group)" :key="spec.key" class="min-w-0 space-y-1">
          <label :for="`${baseId}-${spec.key}`" class="field-label">{{ spec.label }}</label>
          <input v-if="spec.kind === 'boolean'" :id="`${baseId}-${spec.key}`" type="checkbox" class="h-4 w-4 accent-accent"
            :checked="valueOf(spec) === true" :disabled="disabled" @change="set(spec.key, ($event.target as HTMLInputElement).checked)" />
          <input v-else :id="`${baseId}-${spec.key}`" :type="spec.kind === 'integer' || spec.kind === 'number' ? 'number' : spec.kind === 'password' ? 'password' : 'text'"
            :value="valueOf(spec)" :min="spec.min" :max="spec.max" :step="spec.kind === 'number' ? 'any' : 1"
            :maxlength="spec.kind === 'text' || spec.kind === 'password' ? spec.max : undefined" :disabled="disabled" class="field"
            autocomplete="off" :aria-invalid="problem(spec) ? true : undefined" :aria-describedby="`${baseId}-${spec.key}-hint`"
            @input="input(spec, $event)" />
          <p :id="`${baseId}-${spec.key}-hint`" class="text-xs" :class="problem(spec) ? 'text-danger' : 'text-ink-muted'">
            {{ problem(spec) || spec.note }}<span v-if="live && !problem(spec)">{{ spec.command ? ' Applies now and saves for restarts.' : ' Takes effect on the next launch.' }}</span>
          </p>
        </div>
      </div>
    </fieldset>
    <fieldset v-if="matches('classes weapons availability') || classes.some(matches) || selectableWeapons.some((w) => matches(w.name))" class="rounded-panel border border-border p-4 space-y-3">
      <legend class="px-2 font-medium">Classes and weapons</legend>
      <p v-if="live" class="text-xs text-ink-muted">Class restrictions take effect on the next launch. Weapon changes apply now and save for restarts.</p>
      <div class="flex flex-wrap gap-4">
        <label v-for="name in classes" :key="name" class="flex items-center gap-2 text-sm capitalize">
          <input type="checkbox" class="accent-accent" :checked="nested('classes')[name] !== false" :disabled="disabled" @change="setNested('classes', name, ($event.target as HTMLInputElement).checked)" />{{ name }}
        </label>
      </div>
      <div v-if="selectableWeapons.length" class="grid gap-3 sm:grid-cols-2 lg:grid-cols-3">
        <label v-for="weapon in selectableWeapons" :key="weapon.name" class="text-sm">{{ weapon.name }}
          <select class="field mt-1" :disabled="disabled" :value="nested('weapons')[weapon.name] ?? 'always'" @change="setNested('weapons', weapon.name, ($event.target as HTMLSelectElement).value)">
            <option value="always">Always available</option><option value="armory">Armory only</option><option value="never">Unavailable</option>
          </select>
        </label>
      </div>
      <label v-if="selectableWeapons.length" class="block text-sm">Set all weapons
        <select class="field mt-1 w-auto" :disabled="disabled" value="" @change="setAllWeapons(($event.target as HTMLSelectElement).value); ($event.target as HTMLSelectElement).value = ''">
          <option value="" disabled>Choose availability</option><option value="always">Always available</option><option value="armory">Armory only</option><option value="never">Unavailable</option>
        </select>
      </label>
      <p v-else class="text-xs text-ink-muted">This game has no weapon catalog available.</p>
    </fieldset>
  </div>
</template>

<script setup lang="ts">
import { computed, ref } from 'vue';
import { settingProblem, type ServerSetting, type ServerOptions, type ServerWeapon, type SettingValue } from '../utils/serverConfiguration';
const props = withDefaults(defineProps<{ modelValue: ServerOptions; schema: ServerSetting[]; weapons?: ServerWeapon[]; disabled?: boolean; live?: boolean; baseId: string }>(), { weapons: () => [] });
const emit = defineEmits<{ (e: 'update:modelValue', value: ServerOptions): void }>();
const search = ref('');
const classes = ['medic', 'sniper', 'gunner', 'rifleman', 'engineer'];
const matches = (text: string) => !search.value || text.toLowerCase().includes(search.value.toLowerCase());
const fields = computed(() => props.schema.filter((s) => matches(`${s.label} ${s.group} ${s.key}`)));
const groups = computed(() => [...new Set(fields.value.map((s) => s.group))]);
const selectableWeapons = computed(() => props.weapons.filter((w) => w.selectable));
const valueOf = (s: ServerSetting) => props.modelValue[s.key] ?? s.default;
const problem = (s: ServerSetting) => settingProblem(s, valueOf(s));
function nested(key: string): Record<string, string | boolean> {
  const value = props.modelValue[key]; return value && typeof value === 'object' ? value : {};
}
function set(key: string, value: SettingValue) { emit('update:modelValue', { ...props.modelValue, [key]: value }); }
function setNested(key: string, name: string, value: string | boolean) { emit('update:modelValue', { ...props.modelValue, [key]: { ...nested(key), [name]: value } }); }
function setAllWeapons(value: string) { emit('update:modelValue', { ...props.modelValue, weapons: Object.fromEntries(selectableWeapons.value.map((weapon) => [weapon.name, value])) }); }
function input(spec: ServerSetting, event: Event) {
  const text = (event.target as HTMLInputElement).value;
  set(spec.key, spec.kind === 'integer' || spec.kind === 'number' ? text === '' ? '' : Number(text) : text);
}
</script>
