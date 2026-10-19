<template>
  <div>
    <!-- Hero: OpenNova, over a map running in it. -->
    <section class="relative isolate overflow-hidden border-b border-border" aria-labelledby="hero-title">
      <img
        :src="hero.src"
        :alt="hero.alt"
        class="absolute inset-0 -z-10 h-full w-full object-cover object-center opacity-60"
        width="1920"
        height="1080"
        fetchpriority="high"
      />
      <div class="absolute inset-0 -z-10 bg-gradient-to-r from-surface via-surface/90 to-surface/30" aria-hidden="true"></div>
      <div class="absolute inset-x-0 bottom-0 -z-10 h-24 bg-gradient-to-t from-surface to-transparent" aria-hidden="true"></div>

      <div class="page py-20 sm:py-28 lg:py-32">
        <div class="max-w-2xl">
          <p class="eyebrow">Open source · Pre-1.0</p>
          <h1 id="hero-title" class="mt-4 text-5xl font-bold leading-tight text-ink-bright sm:text-6xl lg:text-7xl">OpenNova</h1>
          <p class="mt-4 text-2xl font-semibold text-ink-bright sm:text-3xl">A game on NovaLogic's engine, rebuilt in the open.</p>
          <p class="mt-6 text-lg text-ink">
            Play our own game, make yours with the OpenNova Editor, or use OpenNova in place of Jointops.exe with the
            Joint Operations you own.
          </p>
          <p class="mt-4 text-base text-ink-muted">
            Our own NovaWorld keeps Joint Operations and Delta Force Xtreme 2 online, and a
            <RouterLink to="/support" class="link">support tier</RouterLink> hosts a server of your own on it.
          </p>

          <div class="mt-8 flex flex-wrap items-center gap-3">
            <a href="#downloads" class="btn-primary">Download</a>
            <a href="#editor" class="btn-secondary">Make your own</a>
            <RouterLink to="/support" class="btn-secondary">Host a server</RouterLink>
          </div>
          <p class="mt-3 text-sm text-ink-muted">Windows x64. Pre-1.0 and experimental.</p>

          <p class="mt-8 inline-flex items-center gap-2 rounded-full border border-border-strong bg-surface/80 px-3 py-1.5 text-sm text-ink-muted" role="status">
            <span class="h-2 w-2 shrink-0 rounded-full" :class="statusDot" aria-hidden="true"></span>
            <span>{{ statusText }}</span>
          </p>
        </div>
      </div>
    </section>

    <!-- Our own game: what a download plays with no retail install. -->
    <section id="game" class="page scroll-mt-20 py-16 sm:py-20" aria-labelledby="game-title">
      <p class="eyebrow">Our game</p>
      <h2 id="game-title" class="mt-3 max-w-2xl text-3xl font-semibold text-ink-bright">Indigo Shore, and more to come</h2>
      <div class="mt-3 max-w-2xl space-y-3 text-ink-muted">
        <p>
          OpenNova comes with a small game of its own, made in the OpenNova Editor with no file from the original
          games. Land on a jungle island in the rain and clear a rebel camp, alone or in Co-op over LAN, then play it
          again at night in a storm with the Indigo Storm expansion.
        </p>
        <p>
          A carbine, a pistol, a knife, grenades and binoculars; rebels who patrol, stand guard and raise the alarm.
          It is small and growing. Online play in our own game is coming.
        </p>
      </div>
    </section>

    <!-- The editor that made it. -->
    <section id="editor" class="scroll-mt-20 border-y border-border bg-panel/40 py-16 sm:py-20" aria-labelledby="editor-title">
      <div class="page">
        <p class="eyebrow">The OpenNova Editor</p>
        <h2 id="editor-title" class="mt-3 text-3xl font-semibold text-ink-bright">Make your own</h2>
        <p class="mt-3 max-w-2xl text-ink-muted">
          The editor that built our game. Make maps, missions and whole games in the formats Joint Operations reads,
          then Build them and Play them in OpenNova or in the original game.
        </p>
        <div class="mt-10 grid gap-8 lg:grid-cols-5">
          <figure class="lg:col-span-3">
            <video
              class="aspect-video w-full rounded-panel border border-border bg-panel"
              :poster="timelapse.poster"
              controls
              muted
              loop
              playsinline
              preload="none"
              width="1280"
              height="720"
              :aria-label="timelapse.alt"
            >
              <source v-for="source in timelapse.sources" :key="source.src" :src="source.src" :type="source.type" />
            </video>
            <figcaption class="mt-2 text-sm text-ink-muted">{{ timelapse.caption }}</figcaption>
          </figure>
          <div class="lg:col-span-2">
            <ul class="space-y-3">
              <li v-for="feature in editorFeatures" :key="feature" class="flex gap-3 text-ink">
                <span class="mt-2 h-1.5 w-1.5 shrink-0 rounded-full bg-accent" aria-hidden="true"></span>
                <span>{{ feature }}</span>
              </li>
            </ul>
          </div>
        </div>
        <div class="mt-8 grid gap-4 sm:grid-cols-3">
          <figure v-for="shot in editorShots" :key="shot.src">
            <a :href="shot.src" target="_blank" rel="noopener" class="block overflow-hidden rounded-panel border border-border">
              <img :src="shot.thumb" :alt="shot.alt" loading="lazy" decoding="async" width="800" height="450" class="aspect-video w-full object-cover" />
            </a>
            <figcaption class="mt-2 text-sm text-ink-muted">{{ shot.caption }}</figcaption>
          </figure>
        </div>
      </div>
    </section>

    <!-- Retail Joint Operations, run by OpenNova. -->
    <section id="retail" class="page scroll-mt-20 py-16 sm:py-20" aria-labelledby="retail-title">
      <p class="eyebrow">Joint Operations</p>
      <h2 id="retail-title" class="mt-3 text-3xl font-semibold text-ink-bright">Your Joint Operations, too</h2>
      <div class="mt-3 max-w-2xl space-y-3 text-ink-muted">
        <p>
          OpenNova runs in place of Jointops.exe. Press PLAY RETAIL, pick the folder of the Joint Operations you own,
          and OpenNova plays its maps, missions, models and sounds. It ships no retail files.
        </p>
        <p>
          The aim is parity with the game as it shipped, not a remake. OpenNova is pre-1.0, so expect differences,
          and please <a :href="ISSUES_URL" target="_blank" rel="noopener" class="link">report them on GitHub</a>.
        </p>
      </div>
      <div class="mt-10 grid gap-4 sm:grid-cols-3">
        <figure v-for="(shot, i) in retailShots" :key="shot.src" :class="i === 0 ? 'sm:col-span-3' : ''">
          <a :href="shot.src" target="_blank" rel="noopener" class="block overflow-hidden rounded-panel border border-border">
            <img :src="shot.thumb" :alt="shot.alt" loading="lazy" decoding="async" width="1600" height="900" class="aspect-video w-full object-cover" />
          </a>
          <figcaption class="mt-2 text-sm text-ink-muted">{{ shot.caption }}</figcaption>
        </figure>
      </div>
    </section>

    <!-- Online: OpenNova needs nothing; an original exe needs the Switcher. -->
    <section id="online" class="scroll-mt-20 border-y border-border bg-panel/40 py-16 sm:py-20" aria-labelledby="online-title">
      <div class="page">
        <p class="eyebrow">NovaWorld</p>
        <h2 id="online-title" class="mt-3 text-3xl font-semibold text-ink-bright">Play online</h2>
        <p class="mt-3 max-w-2xl text-ink-muted">
          NovaWorld is the online service these games log in to. We run our own, with accounts, lobbies and the
          in-game server browser. First <RouterLink to="/register" class="link">make a NovaWorld account</RouterLink>:
          the handle and password you log in with in-game. Then pick your way in.
        </p>
        <div class="mt-10 grid gap-4 lg:grid-cols-2">
          <article class="card p-6">
            <h3 class="text-lg font-semibold text-ink-bright">With OpenNova</h3>
            <p class="mt-2 text-ink-muted">
              Open NovaWorld from Joint Operations' multiplayer menu in OpenNova and choose OpenNova. Nothing else to
              install.
            </p>
          </article>
          <article class="card p-6">
            <h3 class="text-lg font-semibold text-ink-bright">With the original Jointops.exe or dfx2.exe</h3>
            <p class="mt-2 text-ink-muted">
              The OpenNova Switcher points the original game at our NovaWorld. It manages one block in the Windows
              hosts file, for gs.novaworld.net, so it asks for administrator rights. Your game files stay as they are,
              and it switches back to NovaLogic's NovaWorld whenever you like.
            </p>
            <a href="#downloads" class="link mt-3 inline-block text-sm">Get the Switcher</a>
          </article>
        </div>
        <p class="mt-6 text-ink-muted">
          Both ways meet in the same lobbies. <RouterLink to="/lobby" class="link">See who is playing</RouterLink>.
        </p>
      </div>
    </section>

    <!-- Hosted servers, and running one yourself. -->
    <section class="page py-16 sm:py-20" aria-labelledby="hosting-title">
      <div class="grid gap-10 lg:grid-cols-2 lg:items-center">
        <div>
          <p class="eyebrow">Server hosting</p>
          <h2 id="hosting-title" class="mt-3 text-3xl font-semibold text-ink-bright">Hosted servers</h2>
          <p class="mt-4 text-ink">
            A monthly support tier funds OpenNova and runs your own servers on NovaWorld. We start them on our machines,
            list them in the game's server browser and keep them running until you stop them. Pick the map rotation
            and game settings, watch the stats and use the server console, all from this site.
          </p>
          <p class="mt-3 text-ink-muted">
            Hosted servers run OpenNova's own server, opennova-serve, not the original game. Retail and OpenNova players
            join them alike. They aim to play exactly like the original servers, but OpenNova is pre-1.0 and we can't
            guarantee that yet.
          </p>
          <div class="mt-6">
            <RouterLink to="/support" class="btn-primary">{{ tiersState === 'open' ? 'Choose a tier' : 'See support tiers' }}</RouterLink>
          </div>
          <p class="mt-4 text-xs text-ink-muted">A tier is support for the project, not the sale of any game.</p>
        </div>

        <div>
          <ul v-if="tiersState === 'open' || tiersState === 'loading'" class="grid grid-cols-3 gap-3" :aria-busy="tiersState === 'loading'">
            <template v-if="tiersState === 'open'">
              <li v-for="row in tierRows" :key="row.tier" class="card px-2 py-5 text-center">
                <p class="text-sm text-ink-muted">Tier {{ row.tier }}</p>
                <p class="mt-1 text-3xl font-semibold text-ink-bright">{{ row.price }}</p>
                <p class="text-sm text-ink-muted">a month</p>
                <p class="mt-3 text-sm text-ink">{{ row.servers }}</p>
              </li>
            </template>
            <li v-for="n in loadingCards" v-else :key="n" class="card h-36 animate-pulse" aria-hidden="true"></li>
          </ul>
          <p v-if="ownMachine" class="mt-3 text-sm text-ink-muted">
            Dedicated machine: {{ ownMachine }} a month for each, one more hosted server on a machine of its own, no tier
            needed.
          </p>
          <p v-if="tiersState === 'closed'" class="card p-6 text-sm text-ink-muted">Support tiers are not open on NovaWorld yet.</p>
          <p v-if="tiersState === 'failed'" class="card p-6 text-sm text-ink-muted">
            The prices did not load. They are on the <RouterLink to="/support" class="link">support tiers</RouterLink> page.
          </p>
        </div>
      </div>

      <RunItYourself id="run-it-yourself" class="mt-10" />
    </section>

    <!-- Downloads. -->
    <section id="downloads" class="scroll-mt-20 border-t border-border bg-panel/40 py-16 sm:py-20" aria-labelledby="downloads-title">
      <div class="page">
        <p class="eyebrow">Downloads</p>
        <h2 id="downloads-title" class="mt-3 text-3xl font-semibold text-ink-bright">Get OpenNova</h2>
        <p class="mt-3 max-w-2xl text-ink-muted">
          Windows x64. Pre-1.0 and experimental. The game comes with OpenNova's own game; playing Joint Operations
          needs your own purchased copy. Each release on GitHub carries these files:
        </p>
        <ul class="mt-10 grid gap-4 sm:grid-cols-2 lg:grid-cols-3">
          <li v-for="item in downloads" :key="item.file" class="card p-5">
            <p class="font-semibold text-ink-bright">{{ item.name }}</p>
            <p class="mt-1 text-sm text-ink-muted">{{ item.body }}</p>
            <p class="mt-3 font-mono text-xs text-ink-muted">{{ item.file }}</p>
          </li>
        </ul>
        <div class="mt-8 flex flex-wrap items-center gap-x-6 gap-y-3">
          <a :href="RELEASES_URL" target="_blank" rel="noopener" class="btn-primary">Download the latest release</a>
          <p class="text-sm text-ink-muted">
            The source, the issues and every release are on
            <a :href="GITHUB_URL" target="_blank" rel="noopener" class="link">GitHub</a>.
          </p>
        </div>
      </div>
    </section>
  </div>
</template>

<script setup lang="ts">
import { computed, onMounted, ref } from 'vue';
import { RouterLink } from 'vue-router';
import { fetchStats } from '../api/stats';
import { fetchTiers } from '../api/billing';
import RunItYourself from '../components/RunItYourself.vue';
import type { TiersResponse } from '../types/billing';
import { formatMoney } from '../utils/format';
import { GITHUB_URL, ISSUES_URL, RELEASES_URL } from '../utils/links';
import { editorShots, hero, retailShots, timelapse } from '../utils/media';

const editorFeatures = [
  'Missions in a 3D view of their terrain, with their event logic beside them',
  'Terrains: heights, surface maps and tiles',
  'Models with their animation clips, and a Blender add-on for .3di models',
  'Definition tables, menus, strings, fonts, sounds and music',
  'Build packs the game archives; Play runs them in OpenNova or the original game',
];

const downloads = [
  {
    name: 'OpenNova',
    body: 'The game: our own game, and a drop-in for Jointops.exe that plays the Joint Operations you own.',
    file: 'opennova-game-windows',
  },
  {
    name: 'OpenNova Editor',
    body: 'The editor that made our game, ready to run on its own.',
    file: 'opennova-editor-windows',
  },
  {
    name: 'Blender add-on',
    body: 'Imports and exports .3di models and their animations in Blender 5.',
    file: 'opennova-blender-addon-windows',
  },
  {
    name: 'Tools and dedicated server',
    body: 'The command-line tools and opennova-serve, the server our hosted servers run. Retail and OpenNova players both join it.',
    file: 'opennova-apps-windows',
  },
  {
    name: 'OpenNova Switcher',
    body: "Only for the original Jointops.exe or dfx2.exe: a tray app that points them at our NovaWorld, NovaLogic's, or an address of your own. OpenNova itself doesn't need it.",
    file: 'opennova-switcher-windows',
  },
];

// NovaWorld's live counts (GET /api/stats), as of the page's load.
const stats = ref<{ players: number; lobbies: number } | null>(null);
const statsFailed = ref(false);

const statusText = computed(() => {
  if (statsFailed.value) return 'NovaWorld status unavailable.';
  if (!stats.value) return 'Checking NovaWorld…';
  const { players, lobbies } = stats.value;
  if (!players && !lobbies) return 'NovaWorld is online. No one is hosting right now.';
  return `${players.toLocaleString()} ${players === 1 ? 'player' : 'players'} across ${lobbies.toLocaleString()} ${
    lobbies === 1 ? 'lobby' : 'lobbies'
  } right now.`;
});

const statusDot = computed(() => {
  if (statsFailed.value) return 'bg-ink-muted';
  if (!stats.value) return 'bg-warn';
  return 'bg-online';
});

// The support tiers (GET /api/billing/tiers): loading, open with prices,
// closed (billing off), or failed to load.
const tiers = ref<TiersResponse | null>(null);
const tiersFailed = ref(false);
const loadingCards = 3;

const tierRows = computed(() => {
  const t = tiers.value;
  if (!t || !t.enabled) return [];
  return t.tiers
    .filter((offer) => offer.amount !== null)
    .map((offer) => ({
      tier: offer.tier,
      price: formatMoney(offer.amount as number, t.currency),
      servers: `${offer.max_servers} hosted server${offer.max_servers === 1 ? '' : 's'}`,
    }));
});

const tiersState = computed<'loading' | 'open' | 'closed' | 'failed'>(() => {
  if (tiersFailed.value) return 'failed';
  if (!tiers.value) return 'loading';
  if (!tiers.value.enabled) return 'closed';
  return tierRows.value.length ? 'open' : 'failed';
});

const ownMachine = computed(() => {
  const t = tiers.value;
  if (tiersState.value !== 'open' || !t?.own_machine || t.own_machine.amount === null) return '';
  return formatMoney(t.own_machine.amount, t.currency);
});

onMounted(() => {
  fetchStats()
    .then((value) => {
      if (value) stats.value = { players: value.players, lobbies: value.lobbies };
      else statsFailed.value = true;
    })
    .catch(() => {
      statsFailed.value = true;
    });
  fetchTiers().then((result) => {
    if (result.ok) tiers.value = result.value;
    else tiersFailed.value = true;
  });
});
</script>
