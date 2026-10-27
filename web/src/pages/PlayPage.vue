<template>
  <section class="page py-10 sm:py-14">
    <header class="max-w-2xl">
      <h1 class="text-4xl font-semibold tracking-tight text-ink-bright sm:text-5xl">Start playing</h1>
      <p class="mt-5 text-base leading-relaxed text-ink-muted">Pick the game you want to play and follow its setup.</p>
    </header>
    <fieldset class="mt-9">
      <legend class="text-sm font-semibold text-ink">Which game?</legend>
      <div class="mt-3 grid gap-3 sm:grid-cols-3">
        <label v-for="option in choices" :key="option.id" class="card flex cursor-pointer items-start gap-3 p-5 transition hover:border-accent" :class="choice === option.id ? 'border-accent bg-accent/5' : ''">
          <input v-model="choice" type="radio" name="game" :value="option.id" class="mt-1 h-4 w-4 shrink-0 accent-accent" />
          <span>
            <span class="block font-semibold text-ink-bright">{{ option.name }}</span>
            <span class="mt-1 block text-sm text-ink-muted">{{ option.detail }}</span>
          </span>
        </label>
      </div>
    </fieldset>
    <div class="mt-8 grid gap-8 lg:grid-cols-[1.1fr_1fr]">
      <section class="card p-6 sm:p-8" aria-labelledby="setup-title">
        <p class="eyebrow">Your setup</p>
        <h2 id="setup-title" class="mt-3 text-2xl font-semibold text-ink-bright">{{ active.name }}</h2>
        <template v-if="choice === 'opennova'">
          <p class="mt-4 leading-relaxed text-ink-muted">OP: Acid Gambit is OpenNova's own game, made without a single file from the original games. Clear a rebel camp on a jungle island, then play its expansion, OP: Nifty Package, at night in a storm.</p>
          <ol class="mt-6 list-decimal space-y-3 pl-5 text-sm text-ink">
            <li>Download OpenNova for Windows x64.</li>
            <li>Unzip it into an empty folder and run <code>opennova.exe</code>. The first launch downloads the game.</li>
            <li>Play alone, in co-op over LAN, or online through our NovaWorld.</li>
          </ol>
          <RouterLink to="/downloads#opennova" class="btn-primary mt-7">Download OpenNova</RouterLink>
        </template>
        <template v-else-if="choice === 'jointops'">
          <p class="mt-4 leading-relaxed text-ink-muted">Play the Joint Operations you own. OpenNova runs in place of Jointops.exe and ships no files from the original game. It aims to play exactly like the original, not to remake it.</p>
          <ol class="mt-6 list-decimal space-y-3 pl-5 text-sm text-ink">
            <li>You need your own purchased copy of Joint Operations.</li>
            <li>Download the Joint Operations build, unzip it into an empty folder and run <code>opennova.exe</code>. Its first launch downloads Joint Operations and Escalation.</li>
            <li>Already have Joint Operations installed? <code>opennova.exe</code> is a drop-in replacement for <code>Jointops.exe</code>: copy the build's files into your install's folder and run <code>opennova.exe</code> instead. It plays that install's maps, missions, models and sounds.</li>
          </ol>
          <RouterLink to="/downloads#jointops" class="btn-primary mt-7">Get the Joint Operations build</RouterLink>
        </template>
        <template v-else>
          <p class="mt-4 leading-relaxed text-ink-muted">Play Delta Force Xtreme 2 online again. NovaLogic's NovaWorld for it is down, and OpenNova plays on ours.</p>
          <ol class="mt-6 list-decimal space-y-3 pl-5 text-sm text-ink">
            <li>Download the Delta Force Xtreme 2 build, unzip it into an empty folder and run <code>opennova.exe</code>. Its first launch downloads Delta Force Xtreme 2.</li>
            <li>To play online, open NovaWorld in the multiplayer menu and choose OpenNova.</li>
          </ol>
          <RouterLink to="/downloads#dfx2" class="btn-primary mt-7">Get the Delta Force Xtreme 2 build</RouterLink>
        </template>
        <p class="mt-6 border-t border-border pt-4 text-xs text-ink-muted">OpenNova is pre-1.0 and experimental. Expect differences, and <a :href="ISSUES_URL" target="_blank" rel="noopener" class="link">report problems on GitHub</a>.</p>
      </section>
      <section id="online" class="scroll-mt-24 space-y-5" aria-labelledby="online-title">
        <img :src="retailShots[0].thumb" :alt="retailShots[0].alt" class="aspect-video w-full rounded-panel border border-border object-cover" width="1600" height="900" />
        <div>
          <h2 id="online-title" class="text-2xl font-semibold text-ink-bright">Playing online</h2>
        </div>
        <p class="text-sm leading-relaxed text-ink-muted">NovaWorld is the online service your game signs in to. OpenNova runs its own, and NovaLogic still runs theirs for Joint Operations. An account on one does not work on the other.</p>
        <div class="space-y-4 text-sm">
          <div>
            <h3 class="font-semibold text-ink">With OpenNova</h3>
            <p class="mt-1 leading-relaxed text-ink-muted">In the multiplayer menu, open NovaWorld and choose OpenNova, or Original NovaWorld for NovaLogic's. Nothing else to install.</p>
          </div>
          <div>
            <h3 class="font-semibold text-ink">With the original Jointops.exe</h3>
            <p class="mt-1 leading-relaxed text-ink-muted">Sign in to NovaLogic's NovaWorld as usual. Joint Operations servers we host can list there too, so they show up in the game's server browser.</p>
          </div>
          <div>
            <h3 class="font-semibold text-ink">With the original dfx2.exe</h3>
            <p class="mt-1 leading-relaxed text-ink-muted">It can still reach our NovaWorld: add one hosts-file line that points <code>gs.novaworld.net</code> at the address of <code>{{ NOVAWORLD_GATE_HOST }}</code> (<a :href="HOSTS_LINE_URL" target="_blank" rel="noopener" class="link">how to add the line</a>), then sign in with your OpenNova account.</p>
          </div>
        </div>
        <div class="card p-5">
          <h3 class="font-semibold text-ink">New to our NovaWorld?</h3>
          <p class="mt-2 text-sm text-ink-muted">Create an account here; the game signs in with the same username and password. Browse servers here, then join from your game's server browser.</p>
          <div class="mt-4 flex flex-wrap gap-4">
            <RouterLink to="/register" class="btn-secondary">Create an account</RouterLink>
            <RouterLink to="/lobby" class="btn-secondary">Browse servers</RouterLink>
          </div>
        </div>
      </section>
    </div>
  </section>
</template>
<script setup lang="ts">
import { computed } from 'vue';
import { RouterLink, useRoute, useRouter } from 'vue-router';
import { retailShots } from '../utils/media';
import { HOSTS_LINE_URL, ISSUES_URL, NOVAWORLD_GATE_HOST } from '../utils/links';
const choices = [
  { id: 'opennova', name: 'OP: Acid Gambit', detail: 'Our own game.' },
  { id: 'jointops', name: 'Joint Operations', detail: 'The copy you own, run by OpenNova.' },
  { id: 'dfx2', name: 'Delta Force Xtreme 2', detail: 'Your original game, back online.' },
];
const route = useRoute();
const router = useRouter();
const choice = computed({
  get: () => choices.some((item) => item.id === route.query.game) ? String(route.query.game) : 'opennova',
  set: (game: string) => { void router.replace({ query: { ...route.query, game: game === 'opennova' ? undefined : game }, hash: route.hash }); },
});
const active = computed(() => choices.find((item) => item.id === choice.value)!);
</script>
