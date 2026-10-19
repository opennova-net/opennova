<template>
  <section class="space-y-4" :aria-labelledby="`hosted-server-${server.id}-settings`">
    <div class="flex flex-col gap-3 sm:flex-row sm:items-baseline sm:justify-between">
      <h2 :id="`hosted-server-${server.id}-settings`" class="text-lg font-semibold text-ink">Game settings</h2>
      <RouterLink
        v-if="scope === 'owner'"
        :to="{ path: '/me/servers/new', query: { from: String(server.id) } }"
        class="text-sm text-accent hover:text-ink-bright"
      >
        Start a new server with these
      </RouterLink>
    </div>
    <dl class="grid gap-x-6 gap-y-2 rounded-panel border border-border bg-panel p-4 text-sm sm:grid-cols-[auto_1fr]">
      <dt class="text-ink-muted">{{ FIELD_LABELS.game_name }}</dt>
      <dd class="break-words text-ink">{{ server.settings.game_name }}</dd>
      <dt class="text-ink-muted">{{ FIELD_LABELS.data_set_id }}</dt>
      <dd class="text-ink">
        {{ server.data_set?.display_name || server.game_display_name || 'Unknown game' }}
        <span v-if="server.data.base" class="text-ink-muted">{{ server.data.base.version }}</span>
      </dd>
      <dt class="text-ink-muted">{{ FIELD_LABELS.expansion_id }}</dt>
      <dd class="text-ink">
        {{ server.expansion?.display_name || server.expansion_display_name || 'Base game' }}
        <span v-if="server.data.expansion" class="text-ink-muted">{{ server.data.expansion.version }}</span>
      </dd>
      <template v-if="server.address">
        <dt class="text-ink-muted">Static address</dt>
        <dd class="break-all font-mono text-xs text-ink">
          {{ server.address.hostname || server.address.name }}
          <span v-if="serverAddressEndpoint(server.address)" class="text-ink-muted">{{ serverAddressEndpoint(server.address) }}</span>
        </dd>
      </template>
      <dt class="text-ink-muted">{{ FIELD_LABELS.max_players }}</dt>
      <dd class="text-ink">{{ server.settings.max_players }}</dd>
      <dt class="text-ink-muted">{{ FIELD_LABELS.game_password }}</dt>
      <dd class="flex flex-wrap items-center gap-2 text-ink">
        <template v-if="server.settings.game_password">
          <span class="font-mono">{{ showPassword ? server.settings.game_password : '••••••' }}</span>
          <button
            type="button"
            class="text-xs text-accent hover:text-ink-bright"
            :aria-pressed="showPassword ? 'true' : 'false'"
            @click="showPassword = !showPassword"
          >
            {{ showPassword ? 'Hide' : 'Show' }}<span class="sr-only"> the game password</span>
          </button>
        </template>
        <template v-else>None (public)</template>
      </dd>
      <dt class="text-ink-muted">{{ FIELD_LABELS.time_limit }}</dt>
      <dd class="text-ink">{{ server.settings.time_limit }} minutes</dd>
      <dt class="text-ink-muted">{{ FIELD_LABELS.kill_limit }}</dt>
      <dd class="text-ink">{{ limitText(server.settings.kill_limit) }}</dd>
      <dt class="text-ink-muted">{{ FIELD_LABELS.max_score }}</dt>
      <dd class="text-ink">{{ limitText(server.settings.max_score) }}</dd>
      <dt class="text-ink-muted">{{ FIELD_LABELS.respawn }}</dt>
      <dd class="text-ink">{{ server.settings.respawn }} seconds</dd>
      <dt class="text-ink-muted">{{ FIELD_LABELS.start_delay }}</dt>
      <dd class="text-ink">{{ server.settings.start_delay }} seconds</dd>
      <dt class="text-ink-muted">{{ FIELD_LABELS.team_ff }}</dt>
      <dd class="text-ink">{{ server.settings.team_ff ? 'On' : 'Off' }}</dd>
      <dt class="text-ink-muted">{{ FIELD_LABELS.missions }}</dt>
      <dd class="min-w-0 text-ink">
        <ol class="list-inside list-decimal space-y-0.5">
          <li v-for="(mission, index) in server.settings.missions" :key="index" class="break-all">
            <template v-if="titles.get(mission.toLowerCase())">
              {{ titles.get(mission.toLowerCase()) }} <span class="font-mono text-xs text-ink-muted">{{ mission }}</span>
            </template>
            <span v-else class="font-mono text-xs">{{ mission }}</span>
          </li>
        </ol>
      </dd>
    </dl>
  </section>
</template>

<script setup lang="ts">
// A hosted server page's settings tab (the owner's Settings, the admin's Game
// settings): the game settings it was started with (and the static address it
// runs at), and for the owner a new start from them (/me/servers/new?from=<id>).
import { onMounted, ref } from 'vue';
import { RouterLink } from 'vue-router';
import { fetchMyServers } from '../../api/hostedServer';
import type { HostedServer } from '../../types/hostedServer';
import { serverAddressEndpoint } from '../../utils/addressView';
import { FIELD_LABELS } from '../../utils/hostedSettings';

const props = defineProps<{
  server: HostedServer;
  scope: 'owner' | 'admin';
  now: number;
}>();

defineEmits<{
  (e: 'changed', server: HostedServer | null): void;
  (e: 'gone'): void;
  (e: 'session-ended'): void;
}>();

const showPassword = ref(false);

// Each mission's title, as the games the owner may start name them (the
// admin's view lists the files).
const titles = ref(new Map<string, string>());
onMounted(async () => {
  if (props.scope !== 'owner') return;
  const result = await fetchMyServers();
  if (!result.ok) return;
  const named = new Map<string, string>();
  for (const game of result.value.data_sets) {
    for (const m of [...game.missions, ...game.expansions.flatMap((e) => e.missions)]) {
      if (m.title) named.set(m.file.toLowerCase(), m.title);
    }
  }
  titles.value = named;
});

// KillLimit and MaxScore at 500 are the host screen's "no limit".
function limitText(value: number): string {
  return value >= 500 ? 'No limit' : String(value);
}
</script>
