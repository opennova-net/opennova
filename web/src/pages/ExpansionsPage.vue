<template>
  <section class="mx-auto max-w-5xl px-6 pb-20">
    <header class="py-12 text-center">
      <h1 class="text-3xl font-semibold tracking-tight text-ink md:text-4xl">Curated Game Expansions</h1>
      <p class="mt-3 text-base text-ink-muted md:text-lg">
        Every release listed here has been reviewed by the OpenNova team for quality, compatibility, and balance.
      </p>
    </header>

    <div v-if="isLoading" class="flex justify-center py-16">
      <span class="text-sm uppercase tracking-widest text-ink-muted">Loading expansions…</span>
    </div>

    <div v-else-if="errorMessage" class="rounded-panel border border-danger/40 bg-danger/10 p-6 text-danger">
      {{ errorMessage }}
    </div>

    <div v-else class="space-y-12">
      <div v-for="group in groupedExpansions" :key="group.key" class="space-y-4">
        <div class="flex items-baseline justify-between">
          <h2 class="text-xl font-semibold text-ink">{{ group.title }}</h2>
          <p v-if="group.slug" class="text-xs uppercase tracking-widest text-ink-muted">
            {{ group.slug }}
          </p>
        </div>

        <div v-if="group.expansions.length" class="grid gap-6 md:grid-cols-2">
          <article
            v-for="expansion in group.expansions"
            :key="expansion.slug"
            class="flex h-full flex-col rounded-panel border border-border bg-panel p-5"
          >
            <div class="flex flex-1 flex-col gap-3">
              <div class="flex items-start justify-between gap-3">
                <h3 class="text-lg font-semibold text-accent">{{ expansion.displayName }}</h3>
                <span
                  v-if="expansion.featured"
                  class="inline-flex items-center gap-1 rounded-control bg-accent px-3 py-0.5 text-xs font-semibold uppercase tracking-widest text-on-accent"
                >
                  <span aria-hidden="true">★</span>
                  Featured
                </span>
              </div>
              <p v-if="expansion.summary" class="text-sm text-ink-muted">
                {{ expansion.summary }}
              </p>

              <dl class="grid grid-cols-2 gap-3 text-sm text-ink-muted">
                <div>
                  <dt class="text-xs uppercase tracking-widest text-ink-muted">Version</dt>
                  <dd class="font-mono text-sm text-ink">{{ expansion.version }}</dd>
                </div>
                <div v-if="expansion.install?.target">
                  <dt class="text-xs uppercase tracking-widest text-ink-muted">Install Target</dt>
                  <dd class="font-mono text-sm text-ink">{{ expansion.install.target }}</dd>
                </div>
                <div v-if="expansion.packageType">
                  <dt class="text-xs uppercase tracking-widest text-ink-muted">Package</dt>
                  <dd class="text-sm">{{ expansion.packageType }}</dd>
                </div>
              </dl>

              <div v-if="hasReleaseNotes(expansion)" class="mt-3 text-sm">
                <button
                  type="button"
                  class="inline-flex items-center rounded-control border border-border bg-raised px-3 py-1 text-xs font-semibold uppercase tracking-widest text-accent transition hover:bg-hover"
                  @click="toggleNotes(expansion.slug)"
                >
                  <span v-if="isNotesOpen(expansion.slug)">Hide release notes</span>
                  <span v-else>View release notes</span>
                </button>
                <div
                  v-if="isNotesOpen(expansion.slug)"
                  class="mt-2 whitespace-pre-wrap rounded-panel border border-border bg-surface p-3 text-ink"
                >
                  {{ expansion.releaseNotes }}
                </div>
              </div>

              <RouterLink
                :to="detailPath(expansion.slug)"
                class="mt-auto inline-flex items-center justify-center rounded-control bg-accent px-4 py-2 text-sm font-semibold text-on-accent transition hover:bg-accent/90"
              >
                View expansion details
              </RouterLink>
            </div>
          </article>
        </div>

        <div v-else class="rounded-panel border border-border bg-panel p-6 text-sm text-ink-muted">
          No curated expansions are available for this title yet. Check back as we review new submissions.
        </div>
      </div>

      <p v-if="!groupedExpansions.length" class="text-center text-sm text-ink-muted">
        No curated expansions are published right now. New releases will appear here once they pass review.
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

onMounted(async () => {
  try {
    const [gameResponse, expansionList] = await Promise.all([
      fetchGames(),
      fetchExpansions(),
    ]);

    games.value = gameResponse.games ?? [];
    standaloneExpansions.value = expansionList.filter((expansion) => !expansion.gameSlug);
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

  return groups.sort((a, b) => (a.title ?? '').localeCompare(b.title ?? ''));
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

function detailPath(slug: string): string {
  return `/expansions/${slug}`;
}

function sortExpansions(list: readonly ExpansionSummary[]): ExpansionSummary[] {
  return [...list].sort((a, b) => {
    const featuredDelta = Number(b.featured) - Number(a.featured);
    if (featuredDelta !== 0) {
      return featuredDelta;
    }
    return (a.displayName ?? '').localeCompare(b.displayName ?? '');
  });
}
</script>
