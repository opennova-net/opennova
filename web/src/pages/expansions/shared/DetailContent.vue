<template>
  <div class="space-y-10">
    <div v-if="isLoading" class="flex justify-center py-16">
      <span class="text-sm uppercase tracking-widest text-slate-400">Fetching expansion details…</span>
    </div>

    <div v-else-if="errorMessage" class="rounded-lg border border-red-500/40 bg-red-500/10 p-6 text-red-200">
      {{ errorMessage }}
    </div>

    <article
      v-else-if="expansion"
      class="space-y-8 rounded-2xl border border-white/10 bg-slate-900/70 p-8 shadow"
    >
      <header class="space-y-3">
        <div class="flex flex-wrap items-start justify-between gap-3">
          <h2 class="text-2xl font-semibold tracking-tight text-white md:text-3xl">
            {{ expansion.displayName }}
          </h2>
          <span
            v-if="expansion.featured"
            class="inline-flex items-center gap-1 rounded-full bg-brand-200 px-3 py-0.5 text-xs font-semibold uppercase tracking-widest text-slate-900 shadow-brand"
          >
            <span aria-hidden="true">★</span>
            Featured
          </span>
        </div>
        <p v-if="expansion.summary" class="text-sm text-slate-300 md:text-base">
          {{ expansion.summary }}
        </p>
      </header>

      <div class="space-y-6 md:grid md:grid-cols-2 md:gap-6 md:space-y-0">
        <dl class="space-y-3 text-sm text-slate-200">
          <div>
            <dt class="text-xs uppercase tracking-widest text-slate-500">Game</dt>
            <dd>{{ expansion.game?.displayName ?? 'Standalone content' }}</dd>
          </div>
          <div>
            <dt class="text-xs uppercase tracking-widest text-slate-500">Version</dt>
            <dd class="font-mono text-sm text-white">{{ expansion.version }}</dd>
          </div>
          <div>
            <dt class="text-xs uppercase tracking-widest text-slate-500">Package Type</dt>
            <dd class="capitalize">{{ expansion.packageType }}</dd>
          </div>
          <div>
            <dt class="text-xs uppercase tracking-widest text-slate-500">Install Target</dt>
            <dd class="font-mono text-sm text-white">{{ expansion.install.target }}</dd>
          </div>
        </dl>

        <section class="rounded-lg border border-white/10 bg-slate-950/70 p-6 text-sm text-slate-200">
          <h3 class="text-xs uppercase tracking-widest text-slate-500">How to install</h3>
          <p class="mt-3 text-slate-300">
            Expansions are delivered through the OpenNova launcher. Download the latest build, sign in, and enable this package from the Expansion Manager—we’ll handle the download and staging for you.
          </p>
          <RouterLink
            to="/"
            class="mt-4 inline-flex items-center justify-center rounded-md bg-brand-200 px-4 py-2 text-sm font-semibold text-slate-900 transition hover:bg-brand-100"
          >
            Get the OpenNova Launcher
          </RouterLink>
        </section>
      </div>

      <section class="rounded-lg border border-white/10 bg-slate-950/70 p-6 text-sm text-slate-200">
        <h3 class="text-xs uppercase tracking-widest text-slate-500">Highlights</h3>
        <p class="mt-3 text-slate-300">
          Detailed highlights for this expansion are coming soon. Check back for a deeper dive into maps, gear, and balance notes.
        </p>
      </section>

      <section v-if="hasReleaseNotes" class="rounded-lg border border-white/10 bg-slate-950/70 p-6 text-sm text-slate-200">
        <h3 class="text-xs uppercase tracking-widest text-slate-500">Release Notes</h3>
        <p class="mt-3 whitespace-pre-wrap">{{ expansion.releaseNotes }}</p>
      </section>
    </article>

    <div v-else class="rounded-lg border border-white/10 bg-slate-900/70 p-6 text-sm text-slate-200">
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
<style scoped>
.shadow-brand {
  box-shadow: 0 6px 14px rgba(34, 197, 94, 0.25);
}
</style>
