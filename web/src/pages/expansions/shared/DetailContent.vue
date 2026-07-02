<template>
  <div class="space-y-10">
    <div v-if="isLoading" class="flex justify-center py-16">
      <span class="text-sm uppercase tracking-widest text-ink-muted">Fetching expansion details…</span>
    </div>

    <div v-else-if="errorMessage" class="rounded-panel border border-danger/40 bg-danger/10 p-6 text-danger">
      {{ errorMessage }}
    </div>

    <article
      v-else-if="expansion"
      class="space-y-8 rounded-panel border border-border bg-panel p-8"
    >
      <header class="space-y-3">
        <div class="flex flex-wrap items-start justify-between gap-3">
          <h2 class="text-2xl font-semibold tracking-tight text-ink md:text-3xl">
            {{ expansion.displayName }}
          </h2>
          <span
            v-if="expansion.featured"
            class="inline-flex items-center gap-1 rounded-control bg-accent px-3 py-0.5 text-xs font-semibold uppercase tracking-widest text-on-accent"
          >
            <span aria-hidden="true">★</span>
            Featured
          </span>
        </div>
        <p v-if="expansion.summary" class="text-sm text-ink-muted md:text-base">
          {{ expansion.summary }}
        </p>
      </header>

      <div class="space-y-6 md:grid md:grid-cols-2 md:gap-6 md:space-y-0">
        <dl class="space-y-3 text-sm text-ink">
          <div>
            <dt class="text-xs uppercase tracking-widest text-ink-muted">Game</dt>
            <dd>{{ expansion.game?.displayName ?? 'Standalone content' }}</dd>
          </div>
          <div>
            <dt class="text-xs uppercase tracking-widest text-ink-muted">Version</dt>
            <dd class="font-mono text-sm text-ink">{{ expansion.version }}</dd>
          </div>
          <div>
            <dt class="text-xs uppercase tracking-widest text-ink-muted">Package Type</dt>
            <dd class="capitalize">{{ expansion.packageType }}</dd>
          </div>
          <div>
            <dt class="text-xs uppercase tracking-widest text-ink-muted">Install Target</dt>
            <dd class="font-mono text-sm text-ink">{{ expansion.install.target }}</dd>
          </div>
        </dl>

        <section class="rounded-panel border border-border bg-surface p-6 text-sm text-ink">
          <h3 class="text-xs uppercase tracking-widest text-ink-muted">How to install</h3>
          <p class="mt-3 text-ink-muted">
            Expansions are delivered through the OpenNova launcher. Download the latest build, sign in, and enable this package from the Expansion Manager—we’ll handle the download and staging for you.
          </p>
          <RouterLink
            to="/"
            class="mt-4 inline-flex items-center justify-center rounded-control bg-accent px-4 py-2 text-sm font-semibold text-on-accent transition hover:bg-accent/90"
          >
            Get the OpenNova Launcher
          </RouterLink>
        </section>
      </div>

      <section class="rounded-panel border border-border bg-surface p-6 text-sm text-ink">
        <h3 class="text-xs uppercase tracking-widest text-ink-muted">Highlights</h3>
        <p class="mt-3 text-ink-muted">
          Detailed highlights for this expansion are coming soon. Check back for a deeper dive into maps, gear, and balance notes.
        </p>
      </section>

      <section v-if="hasReleaseNotes" class="rounded-panel border border-border bg-surface p-6 text-sm text-ink">
        <h3 class="text-xs uppercase tracking-widest text-ink-muted">Release Notes</h3>
        <p class="mt-3 whitespace-pre-wrap">{{ expansion.releaseNotes }}</p>
      </section>
    </article>

    <div v-else class="rounded-panel border border-border bg-panel p-6 text-sm text-ink">
      Expansion data is unavailable right now. Please return to the catalog.
    </div>
  </div>
</template>

<script setup lang="ts">
import { computed, onMounted, ref } from 'vue';
import { fetchExpansions } from '../../../api/expansions';
import type { ExpansionSummary } from '../../../types/expansions';

const props = defineProps<{ slug: string }>();

const expansion = ref<ExpansionSummary | null>(null);
const isLoading = ref(true);
const errorMessage = ref<string | null>(null);

onMounted(async () => {
  try {
    const expansions = await fetchExpansions();
    expansion.value = expansions.find((item) => item.slug === props.slug) ?? null;
    if (!expansion.value) {
      errorMessage.value = 'We could not locate that expansion in the catalog.';
    }
  } catch (error) {
    console.error('Failed to load expansion detail', error);
    errorMessage.value = 'Unable to load expansion details right now. Please try again later.';
  } finally {
    isLoading.value = false;
  }
});

const hasReleaseNotes = computed(() => {
  return !!expansion.value?.releaseNotes && expansion.value.releaseNotes.trim().length > 0;
});
</script>
