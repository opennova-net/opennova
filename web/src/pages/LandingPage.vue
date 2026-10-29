<template>
  <div>
    <section class="relative isolate overflow-hidden border-b border-border" aria-labelledby="hero-title">
      <img :src="hero.src" :alt="hero.alt" class="absolute inset-0 -z-10 h-full w-full object-cover opacity-60" width="1920" height="1080" fetchpriority="high" />
      <div class="absolute inset-0 -z-10 bg-gradient-to-r from-surface via-surface/90 to-surface/20" aria-hidden="true"></div>
      <div class="page py-20 sm:py-28">
        <div class="max-w-2xl">
          <h1 id="hero-title" class="text-6xl font-semibold tracking-tight text-ink-bright sm:text-7xl">OpenNova</h1>
          <p class="mt-5 max-w-xl text-2xl font-medium leading-snug text-ink-bright sm:text-3xl">NovaLogic's game engine, rebuilt from scratch, with a game of its own.</p>
          <div class="mt-8 flex flex-wrap gap-3">
            <RouterLink to="/play" class="btn-primary">Play</RouterLink>
            <RouterLink to="/downloads" class="btn-secondary bg-surface/60">Download</RouterLink>
          </div>
        </div>
      </div>
      <div class="border-t border-border-strong bg-surface/80">
        <div class="page flex flex-wrap items-center justify-between gap-3 py-4 text-sm">
          <p role="status" class="text-ink-muted">{{ statusText }}</p>
          <RouterLink to="/lobby" class="entity-link font-medium">Servers <span aria-hidden="true">→</span></RouterLink>
        </div>
      </div>
    </section>

    <section class="page py-14 sm:py-20" aria-labelledby="games-title">
      <h2 id="games-title" class="section-heading mb-6 text-2xl">Games</h2>
      <ul class="grid gap-5 md:grid-cols-3">
        <li v-for="game in games" :id="game.id" :key="game.id" class="card scroll-mt-24 overflow-hidden">
          <img :src="game.shot.src" :alt="game.shot.alt" loading="lazy" width="800" height="450" class="aspect-[16/9] w-full object-cover" />
          <div class="space-y-3 p-6">
            <h3 class="text-xl font-semibold text-ink-bright">{{ game.name }}</h3>
            <p class="text-sm text-ink-muted">{{ game.line }}</p>
            <RouterLink :to="game.to" class="entity-link inline-block font-medium">Play <span aria-hidden="true">→</span></RouterLink>
          </div>
        </li>
      </ul>
    </section>

    <section id="editor" class="scroll-mt-24 border-y border-border bg-panel/40 py-14 sm:py-20" aria-labelledby="editor-title">
      <div class="page space-y-8">
        <div class="flex flex-col gap-6 sm:flex-row sm:items-end sm:justify-between">
          <div class="max-w-2xl space-y-4">
            <h2 id="editor-title" class="text-3xl font-semibold tracking-tight text-ink-bright sm:text-4xl">The OpenNova Editor</h2>
            <p class="text-ink-muted">Shape terrain, build missions, preview models and design menus in the editor we made OP: Acid Gambit with.</p>
          </div>
          <RouterLink to="/downloads#apps" class="btn-secondary shrink-0 self-start sm:self-auto">Get the editor</RouterLink>
        </div>
        <figure class="space-y-4">
          <video class="aspect-video w-full rounded-panel border border-border bg-panel" :poster="editorShowcase.poster" controls muted loop playsinline preload="none" width="1920" height="1080" :aria-label="editorShowcase.alt" aria-describedby="editor-showcase-description">
            <source v-for="source in editorShowcase.sources" :key="source.src" :src="source.src" :type="source.type" />
            <a :href="editorShowcase.sources[1].src">Watch the OpenNova Editor showcase</a>
          </video>
          <figcaption id="editor-showcase-description" class="max-w-3xl text-sm leading-relaxed text-ink-muted">{{ editorShowcase.description }}</figcaption>
        </figure>
      </div>
    </section>
  </div>
</template>

<script setup lang="ts">
// The home page: what OpenNova is, who is playing now, the three games and the
// editor.
import { computed, onMounted, ref } from 'vue';
import { RouterLink } from 'vue-router';
import { fetchStats } from '../api/stats';
import { editorShowcase, gameShots, hero } from '../utils/media';
import { titleName } from '../utils/games';

const games = [
  { id: 'game', name: 'OP: Acid Gambit', line: 'Our own game: clear a rebel camp on a jungle island, alone or in co-op.', to: '/play', shot: gameShots.opennova },
  { id: 'jointops', name: titleName('jo'), line: 'The copy you own, run by OpenNova in place of Jointops.exe.', to: '/play?game=jointops', shot: gameShots.jointops },
  { id: 'dfx2', name: titleName('dfx2'), line: 'Your original game, back online on our NovaWorld.', to: '/play?game=dfx2', shot: gameShots.dfx2 },
];

const stats = ref<{ players: number; lobbies: number } | null>(null);
const failed = ref(false);
const statusText = computed(() => {
  if (failed.value) return 'Player activity is unavailable right now.';
  if (!stats.value) return 'Checking player activity…';
  const { players, lobbies } = stats.value;
  if (!players && !lobbies) return 'No players or servers listed on our NovaWorld right now.';
  return `${players.toLocaleString()} ${players === 1 ? 'player' : 'players'} across ${lobbies.toLocaleString()} ${lobbies === 1 ? 'server' : 'servers'} on our NovaWorld.`;
});
onMounted(async () => {
  try {
    stats.value = await fetchStats();
    failed.value = !stats.value;
  } catch {
    failed.value = true;
  }
});
</script>
