<template>
  <div class="space-y-4">
    <details class="card p-4" @toggle="openSettings">
      <summary class="font-semibold text-ink-bright">Server configuration</summary>
      <div class="mt-4 space-y-4">
        <div class="flex flex-wrap items-center justify-between gap-2">
          <p class="text-sm text-ink-muted">Changes are saved for restarts.</p>
          <button type="button" class="btn-secondary btn-sm" :disabled="busy" @click="reload">Reload</button>
        </div>
        <template v-if="state">
          <ServerSettingsFields v-model="values" :schema="state.schema" :weapons="state.weapons" live :disabled="busy" :base-id="`live-settings-${target.id}`" />
          <p v-if="validation" role="alert" class="text-sm text-danger">{{ validation }}</p>
          <button type="button" class="btn-primary" :disabled="busy || !changed || !!validation" @click="save">Apply and save changes</button>
          <pre v-if="output" class="max-h-48 overflow-auto whitespace-pre-wrap break-words rounded-panel bg-surface p-3 text-xs" aria-live="polite">{{ output }}</pre>
        </template>
      </div>
    </details>
    <p v-if="notice" role="status" class="text-sm">{{ notice }}</p>
    <p v-if="error" role="alert" class="text-sm text-danger">{{ error }}</p>
  </div>
</template>

<script setup lang="ts">
import { computed, ref, watch } from 'vue';
import type { ConsoleTarget } from '../api/hostedServer';
import { configurationError, editConfiguration, readConfiguration, type ConfigurationState } from '../api/serverConfiguration';
import { useConsoleQueue } from '../composables/useConsoleQueue';
import { confirmAction } from '../composables/useConfirm';
import { useUnsavedChanges } from '../composables/useUnsavedChanges';
import { settingProblem, type ServerOptions } from '../utils/serverConfiguration';
import ServerSettingsFields from './ServerSettingsFields.vue';
const props = defineProps<{ target: ConsoleTarget }>();
const queue = useConsoleQueue();
const state = ref<ConfigurationState | null>(null), values = ref<ServerOptions>({}), original = ref<ServerOptions>({});
const busy = ref(false), output = ref(''), error = ref(''), notice = ref('');
const changed = computed(() => JSON.stringify(values.value) !== JSON.stringify(original.value));
const validation = computed(() => {
  for (const spec of state.value?.schema ?? []) {
    const problem = settingProblem(spec, values.value[spec.key] ?? spec.default);
    if (problem) return `${spec.label}: ${problem}`;
  }
  return '';
});
useUnsavedChanges(() => changed.value);
async function load() {
  state.value = await queue.action(() => readConfiguration(props.target));
  const saved = state.value.saved;
  const all: ServerOptions = { ...saved.options };
  for (const spec of state.value.schema) all[spec.key] = spec.core ? (saved as unknown as ServerOptions)[spec.key] ?? spec.default : all[spec.key] ?? spec.default;
  values.value = JSON.parse(JSON.stringify(all)) as ServerOptions;
  original.value = JSON.parse(JSON.stringify(all)) as ServerOptions;
}
async function reload() {
  if (changed.value && !await confirmAction('Discard unapplied edits and reload the server settings?')) return;
  busy.value = true; error.value = '';
  try { await load(); } catch (e) { error.value = configurationError(e); } finally { busy.value = false; }
}
// A nested map (the weapons, the classes) sends only the rows the user changed: a row left
// alone keeps the raw word the server holds for it.
function edited(value: ServerOptions[string], before: ServerOptions[string] | undefined): ServerOptions[string] {
  if (typeof value !== 'object' || typeof before !== 'object') return value;
  return Object.fromEntries(Object.entries(value).filter(([name, v]) => v !== before[name]));
}
function openSettings(event: Event) { if ((event.target as HTMLDetailsElement).open && !state.value && !busy.value) void reload(); }
async function save() {
  if (!state.value || busy.value || validation.value) return;
  const patch: ServerOptions = {}, options: ServerOptions = {};
  for (const [key, value] of Object.entries(values.value)) {
    if (JSON.stringify(value) === JSON.stringify(original.value[key])) continue;
    (state.value.schema.find((s) => s.key === key)?.core ? patch : options)[key] = edited(value, original.value[key]);
  }
  busy.value = true; error.value = ''; notice.value = '';
  try {
    const result = await queue.action(() => editConfiguration(props.target, state.value!.settings_revision, { ...patch, options }));
    output.value = result.replies.join('\n');
    notice.value = result.next_launch_only && result.configuration_saved
      ? 'Saved. The changes apply at the next launch.'
      : result.refused_command
      ? `The server refused "${result.refused_command}". The changes before it were saved; the rest were not sent.`
      : result.configuration_saved ? 'Accepted changes saved. Settings marked for the next launch are pending.' : 'The changes were not saved. Reload before retrying.';
    await load();
  } catch (e) { error.value = configurationError(e); } finally { busy.value = false; }
}
watch(() => `${props.target.kind}:${props.target.id}`, () => { state.value = null; values.value = {}; original.value = {}; output.value = ''; });
</script>
