<template>
  <div class="space-y-6">
    <PageHeader
      :title="plainTitle"
      :crumbs="[{ label: 'Servers', to: '/me/servers' }]"
      refreshable
      :loaded-at="loadedAt"
      :now="now"
      :loading="poll.loading.value"
      @refresh="refresh"
    >
      <template v-if="server" #title><GameText :text="serverName(server.server_name)" /></template>
      <template v-if="server" #meta>
        <span v-if="game" class="server-game">{{ game }}</span>
        <span v-if="server.mission_name" class="server-map"><GameText :text="server.mission_name" /></span>
        <span v-if="gameTypeName(server.game_type)" class="server-game-type"><GameText :text="server.game_type" /></span>
        <span class="server-players">{{ server.players }}/{{ server.max_players }} players</span>
        <span class="server-address font-mono text-xs">{{ server.host_ip }}:{{ server.host_port }}</span>
        <RouterLink :to="`/lobby/${server.rid}`" class="link">Public page</RouterLink>
      </template>
      <template v-if="server" #actions>
        <button type="button" class="btn-danger" :disabled="busy" @click="stop">Stop hosting…</button>
      </template>
    </PageHeader>

    <LiveNotices :message="status" :error="alertText" :poll-trouble="pollTrouble" />

    <p v-if="gone" class="server-gone text-sm text-ink-muted">
      This server is no longer listed. <RouterLink to="/me/servers" class="link">Back to Servers</RouterLink>
    </p>
    <p v-else-if="server === null && !alertText" class="text-sm text-ink-muted">Loading…</p>

    <template v-if="server">
      <!-- Players: PuntPlayer / KillPlayer / ChangeTeam, aimed ByIndex at a
           roster slot -->
      <section class="space-y-2" aria-labelledby="game-players">
        <h2 id="game-players" class="section-heading">Players</h2>
        <div class="table-wrap">
          <table class="data-table">
            <caption class="sr-only">Players on {{ plainTitle }}</caption>
            <thead>
              <tr>
                <th scope="col">Slot</th>
                <th scope="col">Player</th>
                <th scope="col">Team</th>
                <th scope="col"><span class="sr-only">Actions</span></th>
              </tr>
            </thead>
            <tbody>
              <TableState v-if="server.roster.length === 0" :colspan="4" :rows="server.roster" empty="No players." />
              <tr v-for="slot in server.roster" :key="slot.slot">
                <td class="font-mono text-xs">{{ slot.slot }}</td>
                <th scope="row">{{ slot.player_name || '(unnamed)' }}</th>
                <td>{{ teamLabel(slot.team) }}</td>
                <td class="whitespace-nowrap text-right">
                  <button type="button" class="btn-ghost btn-sm" :disabled="busy" @click="playerAction('ChangeTeam', slot)">
                    Switch team<span class="sr-only">, {{ slot.player_name }}</span>
                  </button>
                  <button type="button" class="btn-ghost btn-sm" :disabled="busy" @click="playerAction('KillPlayer', slot)">
                    Kill<span class="sr-only">, {{ slot.player_name }}</span>
                  </button>
                  <button type="button" class="btn-ghost btn-sm" :disabled="busy" @click="playerAction('PuntPlayer', slot)">
                    Kick<span class="sr-only">, {{ slot.player_name }}</span>
                  </button>
                </td>
              </tr>
            </tbody>
          </table>
        </div>
      </section>

      <div class="grid gap-6 lg:grid-cols-2">
        <!-- TextChatServer -->
        <section class="space-y-2" aria-labelledby="game-chat">
          <h2 id="game-chat" class="section-heading">Chat</h2>
          <form class="space-y-1" @submit.prevent="sendChat">
            <label for="game-chat-text" class="field-label">Say to everyone</label>
            <div class="flex gap-2">
              <input
                id="game-chat-text"
                v-model="chat"
                type="text"
                class="field min-w-0 flex-1"
                required
                maxlength="400"
                autocomplete="off"
                :aria-invalid="failedForm === 'chat' ? 'true' : undefined"
              />
              <button type="submit" class="btn-secondary" :disabled="busy">Send</button>
            </div>
          </form>
        </section>

      </div>

      <ServerCommands :server="server" id-base="game" @changed="readSoon" @refused="onRefused" />
    </template>
  </div>
</template>

<script setup lang="ts">
// /me/servers/hosts/<rid>: a game the account hosts from the game itself, by
// its listing (GET /api/me/hosts, every LIST_POLL_MS): its game, map, players
// and address, and the owner's pushes to it (POST /api/me/hosts/<rid>/command):
// the players' team, kill and kick, chat, and ServerCommands' name, message,
// mission end, weather and time; and Stop hosting (POST /api/me/hosts/<rid>/stop). A
// hosted server's listing opens that server's page instead.
import { computed, onUnmounted, ref, watch } from 'vue';
import { RouterLink, useRoute, useRouter } from 'vue-router';
import GameText from '../../components/GameText.vue';
import ServerCommands from '../../components/ServerCommands.vue';
import LiveNotices from '../../components/ui/LiveNotices.vue';
import PageHeader from '../../components/ui/PageHeader.vue';
import TableState from '../../components/ui/TableState.vue';
import { fetchListingGames, listMyServers, sendServerCommand, stopHosting, type HostError } from '../../api/myServers';
import { confirmAction } from '../../composables/useConfirm';
import { usePolling } from '../../composables/usePolling';
import { useSession } from '../../composables/useSession';
import type { MyServer, OwnerVerb, RosterSlot, ServerCommandBody } from '../../types/myServers';
import { cp1252Problem } from '../../utils/cp1252';
import { gameTypeName, serverName } from '../../utils/games';
import { stripGameText } from '../../utils/gameText';

// The listing refreshes itself on this period, and shortly after a push.
const LIST_POLL_MS = 10_000;

const TEAM_NAMES: Record<string, string> = { '1': 'Blue', '2': 'Red', '3': 'Yellow', '4': 'Violet' };

const route = useRoute();
const router = useRouter();
const { refresh: refreshSession } = useSession();

const rid = computed(() => Number(route.params.rid));
// Null until the first read found it, and once it is gone.
const server = ref<MyServer | null>(null);
// The listing is not among the account's (stopped, or never its).
const gone = ref(false);
const game = ref('');
const loadedAt = ref<number | null>(null);
const now = ref(Date.now());
const pollTrouble = ref(false);
const busy = ref(false);
const status = ref('');
const alertText = ref('');
// The form whose field the last refusal was about (a 400 invalid_command, or
// the page's own check): its field is marked invalid.
const failedForm = ref<string | null>(null);
let askedGame = false;

const plainTitle = computed(() => (server.value ? stripGameText(serverName(server.value.server_name)) : 'Server'));

const chat = ref('');
function teamLabel(team: string): string {
  if (!team) return 'None';
  return TEAM_NAMES[team] ?? team;
}

async function onSessionEnded() {
  await refreshSession();
  await router.replace({ path: '/login', query: { redirect: route.fullPath } });
}

async function readGame() {
  if (askedGame) return;
  askedGame = true;
  const found = await fetchListingGames();
  game.value = found?.get(rid.value) ?? '';
}

const poll = usePolling({
  async read(signal, background) {
    const result = await listMyServers(signal);
    if (signal.aborted) return false;
    now.value = Date.now();
    if (result.ok) {
      const row = result.value.find((h) => h.rid === rid.value) ?? null;
      // A hosted server's listing: that server's page.
      if (row && row.hosted_server_id != null) {
        await router.replace(`/me/servers/${row.hosted_server_id}`);
        return false;
      }
      server.value = row;
      gone.value = row === null;
      loadedAt.value = now.value;
      pollTrouble.value = false;
      if (row) void readGame();
      return true;
    }
    if (result.error.code === 'unauthorized') {
      await onSessionEnded();
      return false;
    }
    if (background && server.value !== null) pollTrouble.value = true;
    else alertText.value = result.error.message;
    return false;
  },
  next: () => (gone.value ? null : LIST_POLL_MS),
});

// Another game's page in the same component: start over.
watch(rid, () => {
  server.value = null;
  gone.value = false;
  game.value = '';
  askedGame = false;
  refresh();
});

function refresh() {
  status.value = '';
  alertText.value = '';
  failedForm.value = null;
  void poll.refresh();
}

// After a push: the server's own update carries most changes, so look again
// shortly.
let soonTimer: ReturnType<typeof setTimeout> | undefined;
function readSoon() {
  if (soonTimer) clearTimeout(soonTimer);
  soonTimer = setTimeout(() => void poll.poke(), 1500);
}
onUnmounted(() => {
  if (soonTimer) clearTimeout(soonTimer);
});

// A refusal ServerCommands met: the session ended, or the listing is gone.
function onRefused(error: HostError) {
  if (error.code === 'unauthorized') void onSessionEnded();
  if (error.code === 'unknown_rid' || error.code === 'not_hosting') void poll.readAgain();
}

function fail(form: string, error: HostError) {
  status.value = '';
  alertText.value = error.message;
  failedForm.value = error.code === 'invalid_command' ? form : null;
  if (error.code === 'unauthorized') void onSessionEnded();
  if (error.code === 'unknown_rid' || error.code === 'not_hosting') void poll.readAgain();
}

// The page's own check of a text before it is sent: false, with the reason
// shown and the field marked, when it fails.
function textFits(form: string, text: string, cap?: number): boolean {
  const problem = cp1252Problem(text, cap);
  if (problem === null) return true;
  status.value = '';
  alertText.value = problem;
  failedForm.value = form;
  return false;
}

// One push; true once the service queued it.
async function push(form: string, body: ServerCommandBody, done: string): Promise<boolean> {
  if (busy.value || !server.value) return false;
  busy.value = true;
  status.value = '';
  alertText.value = '';
  failedForm.value = null;
  const result = await sendServerCommand(server.value.rid, body);
  busy.value = false;
  if (!result.ok) {
    fail(form, result.error);
    return false;
  }
  status.value = done;
  readSoon();
  return true;
}

async function sendChat() {
  const text = chat.value.trim();
  if (!text || !textFits('chat', text)) return;
  if (await push('chat', { verb: 'TextChatServer', args: [text] }, `Sent: “${text}”.`)) chat.value = '';
}

async function playerAction(verb: Extract<OwnerVerb, 'PuntPlayer' | 'KillPlayer' | 'ChangeTeam'>, slot: RosterSlot) {
  const who = slot.player_name || `slot ${slot.slot}`;
  const expectedName = slot.player_name;
  const serverRid = rid.value;
  const prompts: Record<typeof verb, string> = {
    PuntPlayer:
      slot.slot === 0
        ? `Kick ${who}? Slot 0 is the host's own player: the hosting ends for everyone.`
        : `Kick ${who} from ${plainTitle.value}?`,
    KillPlayer: `Kill ${who}?`,
    ChangeTeam: `Move ${who} to the other team?`,
  };
  if (!(await confirmAction(prompts[verb]))) return;
  // The roster may have moved on while the question was open.
  const current = server.value?.roster.find((entry) => entry.slot === slot.slot);
  if (rid.value !== serverRid || !current || current.player_name !== expectedName) {
    status.value = '';
    failedForm.value = null;
    alertText.value = 'The player list changed. Choose a player from the current list and try again.';
    return;
  }
  const done: Record<typeof verb, string> = {
    PuntPlayer: `Kicking ${who}.`,
    KillPlayer: `Killing ${who}.`,
    ChangeTeam: `Moving ${who} to the other team.`,
  };
  await push(`player-${slot.slot}`, { verb, target: 'ByIndex', args: [String(slot.slot)] }, done[verb]);
}

// ServerStopHosting (MsgCode 7): the match ends for everyone, and the
// listing goes.
async function stop() {
  const s = server.value;
  if (busy.value || !s) return;
  const label = plainTitle.value;
  if (!(await confirmAction(`Stop hosting ${label}? Its match ends for everyone.`))) return;
  busy.value = true;
  status.value = '';
  alertText.value = '';
  failedForm.value = null;
  const result = await stopHosting(s.rid);
  busy.value = false;
  if (!result.ok) {
    fail('stop', result.error);
    return;
  }
  status.value = `Stopped hosting ${label}.`;
  server.value = null;
  gone.value = true;
}
</script>
