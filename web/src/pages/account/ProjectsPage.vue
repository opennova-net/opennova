<template>
  <div class="space-y-6">
    <PageHeader title="Projects" refreshable :loaded-at="loadedAt" :now="now" :loading="poll.loading.value" @refresh="poll.refresh">
      <template v-if="data" #meta>
        <span id="projects-allowance" :class="atLimit ? 'text-warn' : ''">{{ data.usage.projects }} of {{ data.limits.projects }} projects</span>
        <span aria-hidden="true">·</span>
        <span id="projects-storage" :class="storageFull ? 'text-warn' : ''" :title="`${formatMemory(data.usage.release_bytes)} of it is release packages`">
          {{ formatMemory(data.usage.bytes) }} of {{ formatMemory(data.limits.bytes) }}
        </span>
      </template>
      <template #actions>
        <RouterLink v-if="data && !atLimit" to="/me/projects/new" aria-describedby="projects-allowance" class="btn-primary">New project</RouterLink>
      </template>
    </PageHeader>

    <LiveNotices :error="loadError" />

    <Notice v-if="blocked" tone="warn" class="projects-blocked">{{ blocked }}</Notice>

    <div class="table-wrap">
      <table class="data-table">
        <thead>
          <tr>
            <th scope="col">Project</th>
            <th scope="col">What it is</th>
            <th scope="col">Who gets it</th>
            <th scope="col">Newest</th>
            <th scope="col">Published</th>
            <th scope="col">Stored</th>
          </tr>
        </thead>
        <tbody>
          <TableState
            v-if="!data || data.projects.length === 0"
            :colspan="6"
            :rows="data ? data.projects : null"
            :loading="poll.loading.value"
            empty="No projects yet."
          />
          <template v-else>
            <tr v-for="p in data.projects" :key="p.key" class="project-row">
              <th scope="row">
                <RouterLink :to="myProjectLink(p.key)" class="entity-link break-words">{{ p.title }}</RouterLink>
                <span class="ml-2 font-mono text-xs text-ink-muted">{{ p.key }}</span>
              </th>
              <td>
                {{ projectKindLine(p, data.games) }}
                <span v-if="p.kind === 'expansion' && p.expansion_code" class="ml-1 font-mono text-xs text-ink-muted" title="Short name">{{ p.expansion_code }}</span>
              </td>
              <td><Badge :tone="p.visibility === 'public' ? 'ok' : 'muted'">{{ p.visibility === 'public' ? 'Public' : 'Private' }}</Badge></td>
              <td class="font-mono">{{ p.head > 0 ? `r${p.head}` : 'None' }}</td>
              <td>{{ p.release ? p.release.version || `Release ${p.release.id}` : 'None' }}</td>
              <td class="whitespace-nowrap">{{ p.storage ? formatMemory(p.storage.bytes) : '' }}</td>
            </tr>
          </template>
        </tbody>
      </table>
    </div>
  </div>
</template>

<script setup lang="ts">
// /me/projects: the account's own projects (GET /api/me/projects), a row each
// linking to its page, under a line of what it uses against its limits and
// New project while it may make another. Nothing here changes by itself but a
// push from the editor, so the page reads again when its tab is shown.
import { computed, ref } from 'vue';
import { RouterLink, useRoute, useRouter } from 'vue-router';
import Badge from '../../components/ui/Badge.vue';
import LiveNotices from '../../components/ui/LiveNotices.vue';
import Notice from '../../components/ui/Notice.vue';
import PageHeader from '../../components/ui/PageHeader.vue';
import TableState from '../../components/ui/TableState.vue';
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
const loadedAt = ref<number | null>(null);
const now = ref(Date.now());

const atLimit = computed(() => !!data.value && data.value.usage.projects >= data.value.limits.projects);
const storageFull = computed(() => !!data.value && data.value.usage.bytes >= data.value.limits.bytes);

// Why the account can make or push no more, in a line; '' while it can.
const blocked = computed(() => {
  if (atLimit.value) return `You have ${data.value!.limits.projects} projects, the most an account may have.`;
  if (storageFull.value) return 'Your storage is full: delete a release or a project to push more.';
  return '';
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
    now.value = Date.now();
    loadedAt.value = now.value;
    return true;
  },
  next: () => null,
});
</script>
