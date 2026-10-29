<template>
  <div class="space-y-4">
    <details class="card p-4" @toggle="openSettings">
      <summary class="font-semibold text-ink-bright">Server configuration</summary>
      <div class="mt-4 space-y-4">
        <p class="text-sm text-ink-muted">Changes are saved for restarts. Settings marked for the next launch do not restart the server.</p>
        <button type="button" class="btn-secondary btn-sm" :disabled="busy" @click="reload">Reload saved and live settings</button>
        <template v-if="state">
          <ServerSettingsFields v-model="values" :schema="state.schema" :weapons="state.weapons" live :disabled="busy" :base-id="`live-settings-${target.id}`" />
          <p v-if="validation" role="alert" class="text-sm text-danger">{{ validation }}</p>
          <button type="button" class="btn-primary" :disabled="busy || !changed || !!validation" @click="save">Apply and save changes</button>
          <details class="border-t border-border pt-3">
            <summary class="text-sm font-medium">Current live settings</summary>
            <p class="my-2 text-xs text-ink-muted">These are the TCP interface's live values. Saved settings above may include changes awaiting the next launch.</p>
            <label class="text-xs"><input v-model="showPasswords" type="checkbox" /> Show passwords</label>
            <dl class="mt-2 grid gap-1 text-xs sm:grid-cols-2">
              <div v-for="row in state.settings" :key="row.key" class="flex gap-2"><dt class="font-medium">{{ row.key }}</dt><dd class="break-all font-mono">{{ /password/i.test(row.key) && !showPasswords ? '••••' : row.value }}</dd></div>
            </dl>
          </details>
        </template>
      </div>
    </details>

    <details class="card p-4">
      <summary class="font-semibold text-ink-bright">Advanced admin console</summary>
      <div class="mt-4 space-y-4">
        <p class="text-sm text-ink-muted">Run any command supported by this server's TCP admin interface. Accepted settings, weapon restrictions, and persistent rotation edits are saved automatically. One-shot maps stay temporary.</p>
        <div class="flex flex-wrap gap-2">
          <button v-for="read in reads" :key="read" class="btn-secondary btn-sm" :disabled="busy" @click="run(read)">{{ read }}</button>
        </div>
        <div class="flex flex-wrap gap-2">
          <label class="text-sm">All players
            <select v-model="bulk" class="field mt-1"><option>PUNT</option><option>BAN</option><option>SWAPTEAM</option><option>KILL</option></select>
          </label>
          <button class="btn-warn btn-sm self-end" :disabled="busy" @click="run(`PLAYER ${bulk} ALL`)">Apply to all</button>
          <button class="btn-secondary btn-sm self-end" :disabled="busy" @click="run('GOTO GAMESTATE')">Enter game</button>
          <button class="btn-warn btn-sm self-end" :disabled="busy" @click="run('GOTO MENUSTATE')">End game session</button>
          <button class="btn-warn btn-sm self-end" :disabled="busy" @click="run('MISSION CLEAR')">Clear rotation</button>
          <button class="btn-secondary btn-sm self-end" :disabled="busy" @click="run('QUIT')">Reconnect admin session</button>
        </div>
        <form class="space-y-2" @submit.prevent="run(command)">
          <label :for="`command-${target.id}`" class="field-label">Command</label>
          <div class="flex gap-2"><input :id="`command-${target.id}`" v-model="command" class="field min-w-0 flex-1 font-mono" autocomplete="off" spellcheck="false" placeholder="SET GameTime 30" /><button class="btn-primary" :disabled="busy || !command.trim()">Run</button></div>
        </form>
        <details class="text-sm text-ink-muted"><summary>Command reference</summary>
          <ul class="mt-2 space-y-2 font-mono text-xs">
            <li>SET &lt;setting&gt; &lt;value&gt; · clear a password with SET ServerPassword</li>
            <li>WEAPON SET &lt;index | ALL&gt; &lt;ALWAYS | NEVER | ARMORY&gt;</li>
            <li>MISSION ADD &lt;file.bms&gt; &lt;0 | 1&gt; &lt;insert index&gt; [ONESHOT]</li>
            <li>MISSION REMOVE &lt;index&gt; · MISSION SETNEXT &lt;index&gt; · MISSION CYCLE</li>
            <li>PLAYER &lt;PUNT | BAN | SWAPTEAM | KILL&gt; &lt;slot | ALL&gt;</li>
            <li>CHAT SEND &lt;text&gt; · CMD &lt;server console command&gt; · PETERRABBIT</li>
          </ul>
          <p class="mt-2">QUIT closes the admin connection; it reconnects automatically. ADMINUSER, BANLIST, and SET LevelRestrict are retail stubs and return unsupported responses.</p>
        </details>
        <pre v-if="output" class="max-h-96 overflow-auto whitespace-pre-wrap break-words rounded-panel bg-surface p-3 text-xs" aria-live="polite">{{ output }}</pre>
      </div>
    </details>
    <p v-if="notice" role="status" class="text-sm">{{ notice }}</p>
    <p v-if="error" role="alert" class="text-sm text-danger">{{ error }}</p>
  </div>
</template>

<script setup lang="ts">
import { computed, ref, watch } from 'vue';
import type { ConsoleTarget } from '../api/hostedServer';
import { configurationError, editConfiguration, readConfiguration, sendAdminCommand, type ConfigurationState } from '../api/serverConfiguration';
import { useConsoleQueue } from '../composables/useConsoleQueue';
import { confirmAction } from '../composables/useConfirm';
import { useUnsavedChanges } from '../composables/useUnsavedChanges';
import { settingProblem, type ServerOptions } from '../utils/serverConfiguration';
import ServerSettingsFields from './ServerSettingsFields.vue';
const props = defineProps<{ target: ConsoleTarget }>();
const queue = useConsoleQueue();
const state = ref<ConfigurationState | null>(null), values = ref<ServerOptions>({}), original = ref<ServerOptions>({});
const busy = ref(false), showPasswords = ref(false), command = ref(''), bulk = ref('PUNT'), output = ref(''), error = ref(''), notice = ref('');
const reads = ['GET GAMESTATE', 'GET GAMESETTINGS', 'WEAPON LIST', 'MISSION LIST', 'MISSION AVAILABLE', 'PLAYER LIST', 'CHAT GET'];
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
function openSettings(event: Event) { if ((event.target as HTMLDetailsElement).open && !state.value && !busy.value) void reload(); }
async function save() {
  if (!state.value || busy.value || validation.value) return;
  const patch: ServerOptions = {}, options: ServerOptions = {};
  for (const [key, value] of Object.entries(values.value)) {
    if (JSON.stringify(value) === JSON.stringify(original.value[key])) continue;
    (state.value.schema.find((s) => s.key === key)?.core ? patch : options)[key] = value;
  }
  busy.value = true; error.value = ''; notice.value = '';
  try {
    const result = await queue.action(() => editConfiguration(props.target, state.value!.settings_revision, { ...patch, options }));
    output.value = result.replies.join('\n');
    notice.value = result.configuration_saved ? 'Accepted changes saved. Settings marked for the next launch are pending.' : 'The changes were not saved. Reload before retrying.';
    await load();
  } catch (e) { error.value = configurationError(e); } finally { busy.value = false; }
}
async function run(line: string) {
  if (busy.value || !line.trim()) return;
  if (/^\s*(?:PLAYER\s+(?:PUNT|BAN|KILL)\s+ALL|MISSION\s+(?:CLEAR|CYCLE)|GOTO\s+MENUSTATE|CMD\s+(?:QUIT|EXIT))/i.test(line) &&
      !await confirmAction(`Run “${line}”? This affects the active game and its players.`)) return;
  busy.value = true; error.value = ''; notice.value = '';
  try {
    const result = await queue.action(() => sendAdminCommand(props.target, line));
    output.value = result.replies.join('\n') || 'Command completed.';
    notice.value = result.configuration_saved ? 'Configuration saved for restarts.' : '';
    if (result.configuration_saved && state.value && !changed.value) await load();
  } catch (e) { error.value = configurationError(e); } finally { busy.value = false; }
}
watch(() => `${props.target.kind}:${props.target.id}`, () => { state.value = null; values.value = {}; original.value = {}; output.value = ''; });
</script>
