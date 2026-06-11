<template>
  <section class="mx-auto max-w-5xl px-6 pb-20">
    <header class="py-12 text-center">
      <h1 class="text-3xl font-semibold tracking-tight text-white md:text-4xl">Curated Game Expansions</h1>
      <p class="mt-3 text-base text-slate-300 md:text-lg">
        Every release listed here has been reviewed by the OpenNova team for quality, compatibility, and balance.
      </p>
    </header>

    <div v-if="isLoading" class="flex justify-center py-16">
      <span class="text-sm uppercase tracking-widest text-slate-400">Loading expansions…</span>
    </div>

    <div v-else-if="errorMessage" class="rounded-lg border border-red-500/40 bg-red-500/10 p-6 text-red-200">
      {{ errorMessage }}
    </div>

    <div v-else class="space-y-12">
      <div v-for="group in groupedExpansions" :key="group.key" class="space-y-4">
        <div class="flex items-baseline justify-between">
          <h2 class="text-xl font-semibold text-white">{{ group.title }}</h2>
          <p v-if="group.slug" class="text-xs uppercase tracking-widest text-slate-500">
            {{ group.slug }}
          </p>
        </div>

        <div v-if="group.expansions.length" class="grid gap-6 md:grid-cols-2">
          <article
            v-for="expansion in group.expansions"
            :key="expansion.slug"
            class="flex h-full flex-col rounded-xl border border-white/10 bg-slate-900/70 p-5 shadow-sm"
          >
            <div class="flex flex-1 flex-col gap-3">
              <div class="flex items-start justify-between gap-3">
                <h3 class="text-lg font-semibold text-brand-100">{{ expansion.displayName }}</h3>
                <span
                  v-if="expansion.featured"
                  class="inline-flex items-center gap-1 rounded-full bg-brand-200 px-3 py-0.5 text-xs font-semibold uppercase tracking-widest text-slate-900 shadow-brand"
                >
                  <span aria-hidden="true">★</span>
                  Featured
                </span>
              </div>
              <p v-if="expansion.summary" class="text-sm text-slate-300">
                {{ expansion.summary }}
              </p>

              <dl class="grid grid-cols-2 gap-3 text-sm text-slate-300">
                <div>
                  <dt class="text-xs uppercase tracking-widest text-slate-500">Version</dt>
                  <dd class="font-mono text-sm text-white">{{ expansion.version }}</dd>
                </div>
                <div>
                  <dt class="text-xs uppercase tracking-widest text-slate-500">Install Target</dt>
                  <dd class="font-mono text-sm text-white">{{ expansion.install.target }}</dd>
                </div>
                <div>
                  <dt class="text-xs uppercase tracking-widest text-slate-500">Package</dt>
                  <dd class="text-sm">{{ expansion.packageType }}</dd>
                </div>
              </dl>

              <div v-if="hasReleaseNotes(expansion)" class="mt-3 text-sm">
                <button
                  type="button"
                  class="inline-flex items-center rounded-md border border-white/10 bg-white/5 px-3 py-1 text-xs font-semibold uppercase tracking-widest text-brand-100 transition hover:bg-white/10"
                  @click="toggleNotes(expansion.slug)"
                >
                  <span v-if="isNotesOpen(expansion.slug)">Hide release notes</span>
                  <span v-else>View release notes</span>
                </button>
                <div
                  v-if="isNotesOpen(expansion.slug)"
                  class="mt-2 whitespace-pre-wrap rounded-md border border-white/10 bg-slate-950/70 p-3 text-slate-200"
                >
                  {{ expansion.releaseNotes }}
                </div>
              </div>

              <RouterLink
                v-if="hasDetailPage(expansion.slug)"
                :to="detailPath(expansion.slug)"
                class="mt-auto inline-flex items-center justify-center rounded-md bg-brand-200 px-4 py-2 text-sm font-semibold text-slate-900 transition hover:bg-brand-100"
              >
                View expansion details
              </RouterLink>
            </div>
          </article>
        </div>

        <div v-else class="rounded-lg border border-white/10 bg-slate-900/70 p-6 text-sm text-slate-300">
          No curated expansions are available for this title yet. Check back as we review new submissions.
        </div>
      </div>

      <p v-if="!groupedExpansions.length" class="text-center text-sm text-slate-400">
        No curated expansions are published right now—new releases will appear here once they pass review.
      </p>
    </div>
  </section>
</template>

<script setup lang="ts">
import { computed, onMounted, reactive, ref } from 'vue';
import { fetchExpansions } from '../api/expansions';
import { fetchGames } from '../api/games';
import type { ExpansionSummary } from '../types/expansions';
import type { GameSummary } from '../types/games';

interface ExpansionGroup {
  key: string;
  slug: string | null;
  title: string;
  expansions: ExpansionSummary[];
}

const games = ref<GameSummary[]>([]);
const standaloneExpansions = ref<ExpansionSummary[]>([]);
const isLoading = ref(true);
const errorMessage = ref<string | null>(null);
const openNotes = reactive<Record<string, boolean>>({});
const expansionsWithDetail = new Set(['revx02', 'onjo01', 'ondx01']);

onMounted(async () => {
  try {
    const [gameResponse, expansionList] = await Promise.all([
      fetchGames(),
      fetchExpansions(),
    ]);

    games.value = gameResponse.games ?? [];
    standaloneExpansions.value = expansionList.filter((expansion) => !expansion.game);
  } catch (error) {
    console.error('Failed to load expansions', error);
    errorMessage.value = 'Unable to load expansions right now. Please try again later.';
  } finally {
    isLoading.value = false;
  }
});

const groupedExpansions = computed<ExpansionGroup[]>(() => {
  const groups: ExpansionGroup[] = games.value.map((game) => ({
    key: game.slug,
    slug: game.slug,
    title: game.displayName,
    expansions: sortExpansions(game.expansions ?? []),
  }));

  if (standaloneExpansions.value.length > 0) {
    groups.push({
      key: '__standalone',
      slug: null,
      title: 'Standalone Expansions',
      expansions: sortExpansions(standaloneExpansions.value),
    });
  }

  return groups.sort((a, b) => a.title.localeCompare(b.title));
});

function hasReleaseNotes(expansion: ExpansionSummary): boolean {
  return typeof expansion.releaseNotes === 'string' && expansion.releaseNotes.trim().length > 0;
}

function toggleNotes(slug: string) {
  openNotes[slug] = !openNotes[slug];
}

function isNotesOpen(slug: string): boolean {
  return !!openNotes[slug];
}

function hasDetailPage(slug: string): boolean {
  return expansionsWithDetail.has(slug);
}

function detailPath(slug: string): string {
  return `/expansions/${slug}`;
}

function sortExpansions(list: readonly ExpansionSummary[]): ExpansionSummary[] {
  return [...list].sort((a, b) => {
    const featuredDelta = Number(b.featured) - Number(a.featured);
    if (featuredDelta !== 0) {
      return featuredDelta;
    }
    return a.displayName.localeCompare(b.displayName);
  });
}
</script>

<style scoped>
.shadow-brand {
  box-shadow: 0 6px 14px rgba(34, 197, 94, 0.25);
}
</style>
