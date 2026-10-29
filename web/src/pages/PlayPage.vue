<template>
  <section class="page space-y-8 py-10 sm:py-14">
    <PageHeader title="Play" />

    <fieldset>
      <legend class="field-label">Which game?</legend>
      <div class="grid gap-3 sm:grid-cols-3">
        <label
          v-for="option in choices"
          :key="option.id"
          class="card flex cursor-pointer items-center gap-3 p-4 transition hover:border-accent"
          :class="choice === option.id ? 'border-accent bg-accent/5' : ''"
        >
          <input v-model="choice" type="radio" name="game" :value="option.id" class="h-4 w-4 shrink-0 accent-accent" />
          <span class="font-semibold text-ink-bright">{{ option.name }}</span>
        </label>
      </div>
    </fieldset>

    <div class="grid gap-6 lg:grid-cols-[1.2fr_1fr]">
      <section class="card space-y-5 p-6" aria-labelledby="setup-title">
        <h2 id="setup-title" class="section-heading">{{ active.name }}</h2>
        <p v-if="releaseFacts" id="setup-release" class="text-sm text-ink-muted">{{ releaseFacts }}</p>
        <ol class="list-decimal space-y-2 pl-5 text-sm text-ink">
          <li v-for="step in active.steps" :key="step">{{ step }}</li>
        </ol>
        <RouterLink :to="active.download" class="btn-primary">{{ active.downloadLabel }}</RouterLink>
      </section>

      <section id="online" class="card scroll-mt-24 space-y-3 p-6" aria-labelledby="online-title">
        <h2 id="online-title" class="section-heading">Playing online</h2>
        <p class="text-sm text-ink">In the multiplayer menu, open NovaWorld and choose OpenNova, or Original NovaWorld for NovaLogic's.</p>
        <p class="text-sm text-ink">
          Our NovaWorld signs in with the account you <RouterLink to="/register" class="link">register here</RouterLink>; find a game in
          <RouterLink to="/lobby" class="link">Servers</RouterLink>.
        </p>
        <p class="text-sm text-ink-muted">
          Using the original Jointops.exe or dfx2.exe?
          <a :href="ORIGINAL_GAME_URL" target="_blank" rel="noopener" class="link">Connect it to our NovaWorld</a>
        </p>
      </section>
    </div>
  </section>
</template>

<script setup lang="ts">
// /play: pick a game (?game=, OP: Acid Gambit by default), its setup in a few
// steps and its download, and how to play online.
import { computed, onMounted, ref } from 'vue';
import { RouterLink, useRoute, useRouter } from 'vue-router';
import PageHeader from '../components/ui/PageHeader.vue';
import { downloadsBase, fetchDownloadsManifest, type DownloadsManifest } from '../api/downloads';
import { protocolWords, titleName } from '../utils/games';
import { ORIGINAL_GAME_URL } from '../utils/links';

const choices = [
  {
    id: 'opennova',
    name: 'OP: Acid Gambit',
    // Its product on the downloads host.
    product: 'ACIDGAMBIT',
    steps: ['Download OpenNova.', 'Unzip it into an empty folder and run opennova.exe.', 'Play alone, in co-op, or online.'],
    download: '/downloads#opennova',
    downloadLabel: 'Download OpenNova',
  },
  {
    id: 'jointops',
    name: titleName('jo'),
    product: 'JO',
    steps: [
      'Have your own copy of Joint Operations.',
      'Download the Joint Operations build, unzip it into an empty folder and run opennova.exe.',
      "Or copy the build into your install's folder and run opennova.exe in place of Jointops.exe.",
    ],
    download: '/downloads#jointops',
    downloadLabel: 'Get the Joint Operations build',
  },
  {
    id: 'dfx2',
    name: titleName('dfx2'),
    product: 'DFX2',
    steps: [
      'Download the Delta Force Xtreme 2 build.',
      'Unzip it into an empty folder and run opennova.exe.',
      'Online, open NovaWorld in the multiplayer menu and choose OpenNova.',
    ],
    download: '/downloads#dfx2',
    downloadLabel: 'Get the Delta Force Xtreme 2 build',
  },
];

const route = useRoute();
const router = useRouter();
const choice = computed({
  get: () => (choices.some((item) => item.id === route.query.game) ? String(route.query.game) : 'opennova'),
  set: (game: string) => {
    void router.replace({ query: { ...route.query, game: game === 'opennova' ? undefined : game }, hash: route.hash });
  },
});
const active = computed(() => choices.find((item) => item.id === choice.value)!);

// The picked game's published release as the downloads host lists it: its game version and the
// protocol it speaks ("Game version 0.1.4 · JO protocol 16"); nothing on a site with no host.
const manifest = ref<DownloadsManifest | null>(null);
const releaseFacts = computed(() => {
  const product = manifest.value?.products.find((p) => p.key === active.value.product);
  if (!product?.version) return '';
  return [`Game version ${product.version}`, protocolWords(product.protocol)].filter(Boolean).join(' · ');
});
onMounted(async () => {
  const host = downloadsBase(window.location.hostname);
  if (!host) return;
  try {
    manifest.value = await fetchDownloadsManifest(host);
  } catch {
    manifest.value = null;
  }
});
</script>
