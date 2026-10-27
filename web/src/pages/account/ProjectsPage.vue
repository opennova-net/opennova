<template>
  <div class="space-y-8">
    <header class="flex flex-col gap-4 sm:flex-row sm:items-end sm:justify-between">
      <div>
        <h1 class="text-3xl font-semibold text-ink">Projects</h1>
        <p class="mt-1 max-w-3xl text-sm text-ink-muted">
          Your mods and games on NovaWorld. Make one here or put a project on NovaWorld from the OpenNova Editor, then push
          to it from the editor.
        </p>
      </div>
    </header>

    <p role="alert" class="sr-only">{{ loadError }}</p>

    <div v-if="data === null && !loadError" class="animate-pulse rounded-panel border border-border bg-panel p-8 text-center text-ink-muted">
      Loading your projects…
    </div>

    <div v-else-if="data === null" class="space-y-3 rounded-panel border border-border bg-panel p-6">
      <p aria-hidden="true" class="text-sm text-danger">{{ loadError }}</p>
      <button type="button" class="btn-secondary" @click="poll.refresh">Try again</button>
    </div>

    <template v-else>
      <!-- What the account has against the most it may. -->
      <section class="space-y-4 rounded-panel border border-border bg-panel p-4 sm:p-6" aria-labelledby="projects-usage-heading">
        <h2 id="projects-usage-heading" class="sr-only">Your projects' room</h2>
        <div class="grid gap-6 sm:grid-cols-2">
          <AllowanceMeter
            id="projects-allowance"
            :used="data.usage.projects"
            :max="data.limits.projects"
            singular="project"
            plural="projects"
          />
          <div class="space-y-2">
            <p id="projects-storage" class="text-sm text-ink">
              <span class="text-2xl font-semibold text-ink">{{ formatMemory(data.usage.bytes) }}</span>
              <span class="text-ink-muted"> of </span>
              <span class="font-semibold">{{ formatMemory(data.limits.bytes) }}</span>
              stored
            </p>
            <div aria-hidden="true" class="h-2 overflow-hidden rounded-full" :class="storageFull ? 'bg-warn/20' : 'bg-chart-series/20'">
              <div class="h-full rounded-full" :class="storageFull ? 'bg-warn' : 'bg-chart-series'" :style="{ width: `${storageFraction * 100}%` }"></div>
            </div>
          </div>
        </div>
        <div class="flex flex-col gap-3 border-t border-border pt-4 sm:flex-row sm:items-center sm:justify-between">
          <p class="text-sm" :class="atLimit || storageFull ? 'text-warn' : 'text-ink-muted'">{{ roomLine }}</p>
          <RouterLink v-if="!atLimit" to="/me/projects/new" aria-describedby="projects-allowance" class="btn-primary shrink-0">New project</RouterLink>
        </div>
      </section>

      <p v-if="data.projects.length === 0" class="rounded-panel border border-border bg-panel p-6 text-sm text-ink-muted">
        You have no project on NovaWorld yet.
      </p>

      <ul v-else class="space-y-4">
        <li v-for="p in data.projects" :key="p.key">
          <article class="project-card space-y-2 rounded-panel border border-border bg-panel p-4 sm:p-5" :aria-labelledby="`project-${p.key}-heading`">
            <div class="flex flex-col gap-1 sm:flex-row sm:items-baseline sm:justify-between">
              <h2 :id="`project-${p.key}-heading`" class="min-w-0 break-words text-lg font-semibold">
                <RouterLink :to="myProjectLink(p.key)" class="text-accent hover:text-ink-bright">{{ p.title }}</RouterLink>
              </h2>
              <span class="text-sm font-medium" :class="p.visibility === 'public' ? 'text-online' : 'text-ink-muted'">
                {{ p.visibility === 'public' ? 'Public' : 'Private' }}
              </span>
            </div>
            <p class="text-sm text-ink">
              <span class="font-mono">{{ p.key }}</span>, {{ projectKindLine(p, data.games) }}<template v-if="p.kind === 'expansion' && p.expansion_code">
                (short name <span class="font-mono">{{ p.expansion_code }}</span>)</template>
            </p>
            <p class="text-sm text-ink-muted">
              {{ p.head > 0 ? `Newest revision r${p.head}` : 'Nothing pushed yet' }}<template v-if="p.storage">, {{ formatMemory(p.storage.bytes) }} stored</template>
            </p>
          </article>
        </li>
      </ul>
    </template>
  </div>
</template>

<script setup lang="ts">
// /me/projects: the account's own projects (GET /api/me/projects), each with
// its key, what it is (a mod of a game, or a game), who gets it, its newest
// revision and the bytes it stores, linking to its page (ProjectPage.vue);
// how many it has and the bytes they store against the most it may, and New
// project (NewProjectPage.vue) while it may make another. A mod's game is
// named from the games the same reply lists. Nothing here changes by itself
// but a push from the editor, so the page reads again when its tab is shown.
import { computed, ref } from 'vue';
import { RouterLink, useRoute, useRouter } from 'vue-router';
import AllowanceMeter from '../../components/AllowanceMeter.vue';
import { fetchMyProjects } from '../../api/products';
import { usePolling } from '../../composables/usePolling';
import { useSession } from '../../composables/useSession';
import type { MyProjects } from '../../types/products';
import { formatMemory } from '../../utils/format';
import { myProjectLink, projectKindLine } from '../../utils/projectsView';

const route = useRoute();
const router = useRouter();
const { refresh: refreshSession } = useSession();

const data = ref<MyProjects | null>(null);
const loadError = ref('');

const atLimit = computed(() => !!data.value && data.value.usage.projects >= data.value.limits.projects);
const storageFull = computed(() => !!data.value && data.value.usage.bytes >= data.value.limits.bytes);
const storageFraction = computed(() => {
  const d = data.value;
  return d && d.limits.bytes > 0 ? Math.min(1, d.usage.bytes / d.limits.bytes) : 0;
});

const roomLine = computed(() => {
  const d = data.value;
  if (!d) return '';
  if (atLimit.value) return `You have ${d.limits.projects} projects, the most an account may have. Delete one to make another.`;
  if (storageFull.value) return 'Your projects fill the room NovaWorld gives an account. Delete one to push more.';
  return 'Projects are experimental (pre-1.0).';
});

async function onSessionEnded() {
  await refreshSession();
  await router.replace({ path: '/login', query: { redirect: route.fullPath } });
}

const poll = usePolling({
  async read(signal, background) {
    const mine = await fetchMyProjects(signal);
    if (signal.aborted) return false;
    if (!mine.ok) {
      if (mine.error.code === 'unauthorized') {
        await onSessionEnded();
        return false;
      }
      if (!background || data.value === null) loadError.value = mine.error.message;
      return false;
    }
    data.value = mine.value;
    loadError.value = '';
    return true;
  },
  next: () => null,
});
</script>
