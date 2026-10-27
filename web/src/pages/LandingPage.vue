<template>
  <div>
    <section class="relative isolate overflow-hidden border-b border-border" aria-labelledby="hero-title">
      <img :src="hero.src" :alt="hero.alt" class="absolute inset-0 -z-10 h-full w-full object-cover opacity-60" width="1920" height="1080" fetchpriority="high" />
      <div class="absolute inset-0 -z-10 bg-gradient-to-r from-surface via-surface/90 to-surface/20" aria-hidden="true">
      </div>
      <div class="page py-20 sm:py-28">
        <div class="max-w-2xl">
          <p class="eyebrow">Open source · Pre-1.0</p>
          <h1 id="hero-title" class="mt-5 text-6xl font-semibold tracking-tight text-ink-bright sm:text-7xl">OpenNova</h1>
          <p class="mt-5 max-w-xl text-2xl font-medium leading-snug text-ink-bright sm:text-3xl">NovaLogic's game engine, rebuilt from scratch.</p>
          <p class="mt-5 max-w-lg text-base leading-relaxed text-ink">Play OP: Acid Gambit, our own game, or the Joint Operations you already own. Build missions with the editor we made our game in.</p>
          <div class="mt-8 flex flex-wrap gap-3">
            <RouterLink to="/play" class="btn-primary">Start playing
            </RouterLink>
            <RouterLink to="/lobby" class="btn-secondary bg-surface/60">Find a server</RouterLink>
          </div>
          <p class="mt-4 text-xs text-ink-muted">Windows x64 · Pre-1.0 and experimental</p>
        </div>
      </div>
      <div class="border-t border-border-strong bg-surface/80">
        <div class="page flex flex-wrap items-center justify-between gap-3 py-4 text-sm">
          <p role="status" class="text-ink-muted">{{ statusText }}</p>
          <RouterLink to="/lobby" class="font-medium text-accent hover:text-ink-bright">Browse servers <span aria-hidden="true">→</span>
          </RouterLink>
        </div>
      </div>
    </section>

    <section class="page py-14 sm:py-20" aria-labelledby="choose-title">
      <div class="mb-7 flex flex-wrap items-end justify-between gap-4">
        <div>
          <h2 id="choose-title" class="text-3xl font-semibold tracking-tight text-ink-bright">What you can play</h2>
        </div>
        <RouterLink to="/downloads" class="link text-sm">All downloads</RouterLink>
      </div>
      <div class="grid gap-5 md:grid-cols-3">
        <article id="game" class="card scroll-mt-24 overflow-hidden">
          <img :src="editorShots[0].thumb" :alt="editorShots[0].alt" loading="lazy" width="800" height="450" class="aspect-[16/9] w-full object-cover" />
          <div class="p-6">
            <h3 class="text-xl font-semibold text-ink-bright">OP: Acid Gambit</h3>
            <p class="mt-3 text-sm leading-relaxed text-ink-muted">Our own game, made without a single file from the original games. Clear a rebel camp on a jungle island, alone or in co-op. Its expansion, OP: Nifty Package, plays at night in a storm.</p>
            <RouterLink to="/play" class="mt-5 inline-block font-medium text-accent hover:text-ink-bright">How to play →</RouterLink>
          </div>
        </article>
        <article id="retail" class="card scroll-mt-24 overflow-hidden">
          <img :src="retailShots[0].thumb" :alt="retailShots[0].alt" loading="lazy" width="1600" height="900" class="aspect-[16/9] w-full object-cover" />
          <div class="p-6">
            <h3 class="text-xl font-semibold text-ink-bright">Joint Operations</h3>
            <p class="mt-3 text-sm leading-relaxed text-ink-muted">OpenNova runs the Joint Operations you own in place of Jointops.exe, and aims to play exactly like the original. It is pre-1.0, so expect differences.</p>
            <RouterLink to="/play?game=jointops" class="mt-5 inline-block font-medium text-accent hover:text-ink-bright">Set it up →</RouterLink>
          </div>
        </article>
        <article id="online" class="card scroll-mt-24 flex flex-col p-6">
          <h3 class="text-xl font-semibold text-ink-bright">Servers and hosting</h3>
          <p class="mt-4 text-sm leading-relaxed text-ink-muted">Browse Joint Operations and Delta Force Xtreme 2 servers on our NovaWorld and NovaLogic's, or run your own with a support tier.</p>
          <div class="mt-6 border-t border-border pt-5 text-sm text-ink-muted">Hosted servers run opennova-serve, OpenNova's own server, and take players on the original game and on OpenNova alike. Hosting is experimental. A tier supports the project; it is not the sale of any game.</div>
          <div class="mt-auto flex flex-wrap gap-4 pt-6">
            <RouterLink to="/support" class="font-medium text-accent hover:text-ink-bright">Host a server →</RouterLink>
            <RouterLink to="/play#online" class="link text-sm">How to join</RouterLink>
          </div>
        </article>
      </div>
    </section>

    <section id="editor" class="scroll-mt-24 border-y border-border bg-panel/40 py-14 sm:py-20" aria-labelledby="editor-title">
      <div class="page grid gap-9 lg:grid-cols-2 lg:items-center">
        <div>
          <p class="eyebrow">The OpenNova Editor</p>
          <h2 id="editor-title" class="mt-4 text-3xl font-semibold tracking-tight text-ink-bright sm:text-4xl">Make your own missions</h2>
          <p class="mt-5 max-w-md leading-relaxed text-ink-muted">Shape terrain, place units and script mission logic in a 3D view, then build the files Joint Operations reads. It is the editor we made OP: Acid Gambit with.</p>
          <RouterLink to="/downloads#apps" class="btn-secondary mt-6">Get the editor</RouterLink>
        </div>
        <figure>
          <video class="aspect-video w-full rounded-panel border border-border bg-panel" :poster="timelapse.poster" controls muted loop playsinline preload="none" width="1280" height="720" :aria-label="timelapse.alt">
            <source v-for="source in timelapse.sources" :key="source.src" :src="source.src" :type="source.type" />
          </video>
          <figcaption class="mt-3 text-xs text-ink-muted">{{ timelapse.caption }}</figcaption>
        </figure>
      </div>
    </section>

    <section id="downloads" class="page scroll-mt-24 py-12" aria-labelledby="downloads-title">
      <div class="flex flex-wrap items-center justify-between gap-5">
        <div>
          <h2 id="downloads-title" class="text-2xl font-semibold text-ink-bright">Downloads</h2>
          <p class="mt-2 text-sm text-ink-muted">The games, the editor and the tools, for Windows.</p>
        </div>
        <RouterLink to="/downloads" class="btn-primary">Go to downloads</RouterLink>
      </div>
      <div id="run-it-yourself" class="mt-8 flex flex-wrap gap-x-6 gap-y-3 border-t border-border pt-6 text-sm">
        <RouterLink to="/support#support-run-it-yourself" class="link">Run your own server</RouterLink>
        <a :href="ISSUES_URL" class="link" target="_blank" rel="noopener">Report a problem</a>
      </div>
    </section>
  </div>
</template>

<script setup lang="ts">
import { computed, onMounted, ref } from 'vue';
import { RouterLink } from 'vue-router';
import { fetchStats } from '../api/stats';
import { editorShots, hero, retailShots, timelapse } from '../utils/media';
import { ISSUES_URL } from '../utils/links';
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
  try { stats.value = await fetchStats(); failed.value = !stats.value; }
  catch { failed.value = true; }
});
</script>
