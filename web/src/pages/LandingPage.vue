<template>
  <div>
    <!-- Hero: what OpenNova is, over a retail map running in it. -->
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
          <p class="eyebrow">Open source</p>
          <h1 id="hero-title" class="mt-4 text-4xl font-bold leading-tight text-ink-bright sm:text-5xl lg:text-6xl">
            NovaLogic's engine, rebuilt in the open.
          </h1>
          <p class="mt-6 text-lg text-ink">
            OpenNova is an open-source reimplementation of the engine behind Joint Operations and newer. It plays the
            data from the copy you own, and our own NovaWorld keeps Joint Operations and Delta Force Xtreme 2 online.
          </p>
          <p class="mt-4 text-base text-ink-muted">
            Run your own server on NovaWorld with a
            <RouterLink to="/support" class="link">support tier</RouterLink>: we host it, list it in the game's
            server browser and keep it running.
          </p>

          <div class="mt-8 flex flex-wrap items-center gap-3">
            <a :href="RELEASES_URL" target="_blank" rel="noopener" class="btn-primary">Download OpenNova</a>
            <RouterLink to="/lobby" class="btn-secondary">View lobbies</RouterLink>
            <RouterLink to="/support" class="btn-secondary">Host a server</RouterLink>
          </div>

          <p class="mt-8 inline-flex items-center gap-2 rounded-full border border-border-strong bg-surface/80 px-3 py-1.5 text-sm text-ink-muted" role="status">
            <span class="h-2 w-2 shrink-0 rounded-full" :class="statusDot" aria-hidden="true"></span>
            <span>{{ statusText }}</span>
          </p>
        </div>
      </div>
    </section>

    <!-- What it is, in four parts. -->
    <section class="page py-16 sm:py-20" aria-labelledby="what-title">
      <p class="eyebrow">What OpenNova is</p>
      <h2 id="what-title" class="mt-3 max-w-2xl text-3xl font-semibold text-ink-bright">The original games, on an engine anyone can read.</h2>
      <div class="mt-10 grid gap-4 sm:grid-cols-2">
        <article v-for="pillar in pillars" :key="pillar.title" class="card p-6">
          <h3 class="text-lg font-semibold text-ink-bright">{{ pillar.title }}</h3>
          <p class="mt-2 text-ink-muted">{{ pillar.body }}</p>
        </article>
      </div>
    </section>

    <!-- Retail games running in OpenNova. -->
    <section class="border-y border-border bg-panel/40 py-16 sm:py-20" aria-labelledby="shots-title">
      <div class="page">
        <p class="eyebrow">Screenshots</p>
        <h2 id="shots-title" class="mt-3 text-3xl font-semibold text-ink-bright">Retail games, running in OpenNova</h2>
        <p class="mt-3 max-w-2xl text-ink-muted">
          Every frame here is the original game's own data, loaded and drawn by OpenNova.
        </p>
        <div class="mt-10 grid gap-4 sm:grid-cols-2 lg:grid-cols-3">
          <figure v-for="(shot, i) in gameShots" :key="shot.src" :class="i === 0 ? 'sm:col-span-2 lg:col-span-3' : ''">
            <a :href="shot.src" target="_blank" rel="noopener" class="block overflow-hidden rounded-panel border border-border">
              <img :src="shot.src" :alt="shot.alt" loading="lazy" decoding="async" width="1600" height="900" class="aspect-video w-full object-cover transition duration-300 hover:scale-[1.02]" />
            </a>
            <figcaption class="mt-2 text-sm text-ink-muted">{{ shot.caption }}</figcaption>
          </figure>
        </div>
      </div>
    </section>

    <!-- The editor. -->
    <section class="page py-16 sm:py-20" aria-labelledby="editor-title">
      <p class="eyebrow">Tools</p>
      <h2 id="editor-title" class="mt-3 text-3xl font-semibold text-ink-bright">The OpenNova Editor</h2>
      <p class="mt-3 max-w-2xl text-ink-muted">
        Make maps, missions and whole games for Joint Operations and newer, in the formats the games read.
      </p>
      <div class="mt-10 grid gap-8 lg:grid-cols-5">
        <figure class="lg:col-span-3">
          <video
            ref="timelapseEl"
            class="aspect-video w-full rounded-panel border border-border bg-panel"
            :poster="timelapse.poster"
            muted
            loop
            playsinline
            preload="none"
            :controls="reducedMotion"
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
          <p class="mt-6 text-sm text-ink-muted">Pre-1.0 and experimental, like everything here.</p>
        </div>
      </div>
      <div class="mt-8 grid gap-4 sm:grid-cols-2 lg:grid-cols-3">
        <figure v-for="shot in editorShots" :key="shot.src">
          <a :href="shot.src" target="_blank" rel="noopener" class="block overflow-hidden rounded-panel border border-border">
            <img :src="shot.src" :alt="shot.alt" loading="lazy" decoding="async" width="1600" height="900" class="aspect-video w-full object-cover transition duration-300 hover:scale-[1.03]" />
          </a>
          <figcaption class="mt-2 text-sm text-ink-muted">{{ shot.caption }}</figcaption>
        </figure>
      </div>
    </section>

    <!-- Playing online. -->
    <section class="border-y border-border bg-panel/40 py-16 sm:py-20" aria-labelledby="online-title">
      <div class="page">
        <p class="eyebrow">NovaWorld</p>
        <h2 id="online-title" class="mt-3 text-3xl font-semibold text-ink-bright">Play online today</h2>
        <p class="mt-3 max-w-2xl text-ink-muted">
          Retail Joint Operations and Delta Force Xtreme 2 connect to our NovaWorld as they are. OpenNova joins the same
          servers.
        </p>
        <ol class="mt-10 grid gap-4 sm:grid-cols-2 lg:grid-cols-4">
          <li v-for="(step, i) in steps" :key="step.title" class="card p-6">
            <p class="text-sm font-semibold text-accent">Step {{ i + 1 }}</p>
            <h3 class="mt-1 text-lg font-semibold text-ink-bright">{{ step.title }}</h3>
            <p class="mt-2 text-sm text-ink-muted">{{ step.body }}</p>
            <component
              :is="step.href ? 'a' : RouterLink"
              v-if="step.link"
              v-bind="step.href ? { href: step.href, target: '_blank', rel: 'noopener' } : { to: step.to }"
              class="link mt-3 inline-block text-sm"
            >
              {{ step.link }}
            </component>
          </li>
        </ol>
      </div>
    </section>

    <!-- Managed server hosting through a support tier. -->
    <section class="page py-16 sm:py-20" aria-labelledby="hosting-title">
      <div class="grid gap-10 lg:grid-cols-2 lg:items-center">
        <div>
          <p class="eyebrow">Support tiers</p>
          <h2 id="hosting-title" class="mt-3 text-3xl font-semibold text-ink-bright">Managed server hosting</h2>
          <p class="mt-4 text-ink">
            A monthly support tier funds OpenNova and runs your own servers on NovaWorld. We start them on our machines,
            list them in the game's server browser and keep them running until you stop them. Retail and OpenNova
            players both join.
          </p>
          <p class="mt-3 text-ink-muted">
            Pick the map rotation and game settings, watch the stats and use the server console, all from this site.
          </p>
          <div class="mt-6 flex flex-wrap gap-3">
            <RouterLink to="/support" class="btn-primary">Choose a tier</RouterLink>
          </div>
          <p class="mt-4 text-xs text-ink-muted">
            A tier is support for the project, not the sale of any game. Hosted servers are pre-1.0 and experimental.
          </p>
        </div>

        <div>
          <ul v-if="tierRows.length" class="grid grid-cols-3 gap-3">
            <li v-for="row in tierRows" :key="row.tier" class="card px-2 py-5 text-center">
              <p class="text-sm text-ink-muted">Tier {{ row.tier }}</p>
              <p class="mt-1 text-3xl font-semibold text-ink-bright">{{ row.price }}</p>
              <p class="text-sm text-ink-muted">a month</p>
              <p class="mt-3 text-sm text-ink">{{ row.servers }}</p>
            </li>
          </ul>
          <p v-if="ownMachine" class="mt-3 text-sm text-ink-muted">
            Own machine add-on: {{ ownMachine }} a month for each, to run one of your servers on a machine of its own.
          </p>
          <p v-if="!tierRows.length" class="card p-6 text-sm text-ink-muted">
            Prices and what each tier covers are on the
            <RouterLink to="/support" class="link">support tiers</RouterLink> page.
          </p>
        </div>
      </div>
    </section>

    <!-- Downloads. -->
    <section class="border-t border-border bg-panel/40 py-16 sm:py-20" aria-labelledby="downloads-title">
      <div class="page">
        <p class="eyebrow">Downloads</p>
        <h2 id="downloads-title" class="mt-3 text-3xl font-semibold text-ink-bright">Get OpenNova</h2>
        <p class="mt-3 max-w-2xl text-ink-muted">
          Windows x64. Pre-1.0 and experimental. No game data is included: playing a retail title needs your own
          purchased copy.
        </p>
        <ul class="mt-10 grid gap-4 sm:grid-cols-2 lg:grid-cols-3">
          <li v-for="item in downloads" :key="item.file" class="card p-5">
            <p class="font-semibold text-ink-bright">{{ item.name }}</p>
            <p class="mt-1 text-sm text-ink-muted">{{ item.body }}</p>
            <p class="mt-3 font-mono text-xs text-ink-muted">{{ item.file }}-v*.zip</p>
          </li>
        </ul>
        <div class="mt-8 flex flex-wrap items-center gap-x-6 gap-y-3">
          <a :href="RELEASES_URL" target="_blank" rel="noopener" class="btn-primary">Download the latest release</a>
          <p class="text-sm text-ink-muted">
            The source, the issues and every release are on
            <a :href="GITHUB_URL" target="_blank" rel="noopener" class="link">GitHub</a>.
          </p>
        </div>      </div>
    </section>
  </div>
</template>

<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref } from 'vue';
import { RouterLink } from 'vue-router';
import { fetchStats } from '../api/stats';
import { fetchTiers } from '../api/billing';
import type { TiersResponse } from '../types/billing';
import { formatMoney } from '../utils/format';
import { GITHUB_URL, RELEASES_URL } from '../utils/links';
import { editorShots, gameShots, hero, timelapse } from '../utils/media';

const pillars = [
  {
    title: 'A faithful engine',
    body: "A C++ reimplementation of NovaLogic's engine, ported from how the original works. The aim is parity with the games as they shipped, not a remake.",
  },
  {
    title: 'Your own game data',
    body: 'OpenNova ships no retail files. Point it at the Joint Operations install you own and it plays that data: its maps, missions, models and sounds.',
  },
  {
    title: 'Our own NovaWorld',
    body: "NovaWorld is the online service these games log in to. We run our own, with accounts, lobbies and the in-game server browser, so Delta Force Xtreme 2 activates without NovaLogic's retired service.",
  },
  {
    title: 'Wire-compatible',
    body: 'OpenNova speaks the original network protocol. Retail players join OpenNova servers and OpenNova joins retail ones, on LAN and on NovaWorld.',
  },
];

const editorFeatures = [
  'Missions in a 3D view of their terrain, with their event logic beside them',
  'Terrains: heights, surface maps and tiles',
  'Models with their animation clips, and a Blender add-on for .3di models',
  'Definition tables, menus, strings, fonts, sounds and music',
  'Build packs the game archives; Play runs them in OpenNova or the original game',
];

const steps = [
  {
    title: 'Own the game',
    body: 'Install Joint Operations or Delta Force Xtreme 2 from your own copy.',
  },
  {
    title: 'Make an account',
    body: 'Register a NovaWorld account here. It is the handle and password you log in with in-game.',
    link: 'Create an account',
    to: '/register',
  },
  {
    title: 'Run the Switcher',
    body: 'The OpenNova Switcher points a stock install at our NovaWorld. No patched exe, no injected DLL.',
    link: 'Get the Switcher',
    href: RELEASES_URL,
  },
  {
    title: 'Pick a game',
    body: "Join from the game's own server browser. The Lobbies page shows who is playing now.",
    link: 'View lobbies',
    to: '/lobby',
  },
];

const downloads = [
  {
    name: 'OpenNova',
    body: 'The game. Plays retail Joint Operations from your install, and OpenNova\'s own base game.',
    file: 'opennova-game-windows',
  },
  {
    name: 'OpenNova Editor',
    body: 'The editor, with its own runtime. No Godot install needed.',
    file: 'opennova-editor-windows',
  },
  {
    name: 'OpenNova Switcher',
    body: "A tray app that points a stock Joint Operations or Delta Force Xtreme 2 install at our NovaWorld, NovaLogic's, or an address of your own.",
    file: 'opennova-switcher-windows',
  },
  {
    name: 'Blender add-on',
    body: 'Imports and exports .3di models and their animations in Blender 5.',
    file: 'opennova-blender-addon-windows',
  },
  {
    name: 'Tools and dedicated server',
    body: 'The command-line tools and opennova-serve, a headless server retail and OpenNova clients both join.',
    file: 'opennova-apps-windows',
  },
];

// NovaWorld's live counts (GET /api/stats).
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

// The support tiers (GET /api/billing/tiers); the section links on when they
// are off or did not load.
const tiers = ref<TiersResponse | null>(null);

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

const ownMachine = computed(() => {
  const t = tiers.value;
  if (!t || !t.enabled || !t.own_machine || t.own_machine.amount === null) return '';
  return formatMoney(t.own_machine.amount, t.currency);
});

// The time-lapse plays on its own, muted, unless the visitor asks for reduced
// motion; then it waits with its controls.
const timelapseEl = ref<HTMLVideoElement | null>(null);
const reducedMotion = ref(false);
let observer: IntersectionObserver | null = null;

onMounted(async () => {
  reducedMotion.value = window.matchMedia?.('(prefers-reduced-motion: reduce)').matches ?? false;
  const video = timelapseEl.value;
  if (video && !reducedMotion.value && 'IntersectionObserver' in window) {
    observer = new IntersectionObserver((entries) => {
      for (const entry of entries) {
        if (entry.isIntersecting) void video.play().catch(() => undefined);
        else video.pause();
      }
    });
    observer.observe(video);
  }

  const [statsResult, tiersResult] = await Promise.allSettled([fetchStats(), fetchTiers()]);
  if (statsResult.status === 'fulfilled' && statsResult.value) {
    stats.value = { players: statsResult.value.players, lobbies: statsResult.value.lobbies };
  } else {
    statsFailed.value = true;
  }
  if (tiersResult.status === 'fulfilled' && tiersResult.value.ok) tiers.value = tiersResult.value.value;
});

onBeforeUnmount(() => observer?.disconnect());
</script>
