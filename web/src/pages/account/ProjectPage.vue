<template>
  <div class="space-y-8">
    <header class="space-y-2">
      <RouterLink to="/me/projects" class="text-sm text-accent hover:text-ink-bright">All projects</RouterLink>
      <h1 class="break-words text-3xl font-semibold text-ink">{{ project?.title ?? (missing ? 'No such project' : 'Project') }}</h1>
      <p v-if="project" id="project-kind-line" class="text-sm text-ink-muted">
        <span class="font-mono text-ink">{{ project.key }}</span>, {{ projectKindLine(project, mine?.games ?? []) }}<template v-if="project.kind === 'expansion' && project.expansion_code">
          (short name <span class="font-mono">{{ project.expansion_code }}</span>)</template>
      </p>
    </header>

    <!-- Live regions, always in the page so a screen reader hears what lands in
         them, each its own box while it holds something: the text once. -->
    <div role="status" :class="message ? 'rounded-panel border border-online/40 bg-online/10 p-4 text-sm text-online' : 'sr-only'">{{ message }}</div>
    <div role="alert" :class="error ? 'rounded-panel border border-danger/40 bg-danger/10 p-4 text-sm text-danger' : 'sr-only'">{{ error }}</div>

    <div v-if="missing" class="rounded-panel border border-border bg-panel p-6 text-sm text-ink-muted">
      No project of yours has the key {{ projectKey }}.
      <RouterLink to="/me/projects" class="text-accent hover:text-ink-bright">See your projects</RouterLink>
    </div>
    <div v-else-if="!project" class="animate-pulse rounded-panel border border-border bg-panel p-8 text-center text-ink-muted">
      {{ loadFailed ? 'The project could not be read. This page tries again by itself.' : 'Loading the project…' }}
    </div>

    <template v-else>
      <dl class="grid grid-cols-1 gap-3 sm:grid-cols-3">
        <div class="rounded-panel border border-border bg-panel p-5">
          <dt class="text-xs text-ink-muted">Who gets it</dt>
          <dd id="project-visibility-line" class="mt-2 text-sm text-ink">{{ visibilityLine(project.visibility) }}</dd>
        </div>
        <div class="rounded-panel border border-border bg-panel p-5">
          <dt class="text-xs text-ink-muted">Newest revision</dt>
          <dd class="mt-2 text-xl font-semibold text-ink">{{ project.head > 0 ? `r${project.head}` : 'None yet' }}</dd>
        </div>
        <div v-if="project.storage" class="rounded-panel border border-border bg-panel p-5">
          <dt class="text-xs text-ink-muted">Stored</dt>
          <dd class="mt-2 text-xl font-semibold text-ink">{{ formatMemory(project.storage.bytes) }}</dd>
        </div>
      </dl>

      <div v-if="project.can_manage" class="flex flex-wrap gap-2">
        <button id="project-visibility" type="button" class="btn-secondary" :disabled="busy" @click="toggleVisibility">
          {{ project.visibility === 'public' ? 'Make private' : 'Make public' }}
        </button>
        <button id="project-rename-toggle" type="button" class="btn-secondary" :disabled="busy" @click="toggleRename">
          {{ renaming ? 'Close' : 'Rename…' }}
        </button>
        <button id="project-delete-toggle" type="button" class="btn-danger" :disabled="busy" @click="toggleDelete">Delete…</button>
      </div>
      <p v-else class="text-sm text-ink-muted">Only its owner and NovaWorld's admins may change it.</p>

      <!-- Its name; the key stays. -->
      <form v-if="renaming" id="project-rename-form" class="space-y-2 rounded-panel border border-border bg-panel p-4 sm:p-6" novalidate @submit.prevent="saveRename">
        <label for="project-rename-name" :class="labelClass">Name</label>
        <div class="flex flex-wrap items-center gap-2">
          <input
            id="project-rename-name"
            v-model="newName"
            type="text"
            autocomplete="off"
            :maxlength="NAME_MAX"
            :disabled="busy"
            :aria-invalid="renameProblem ? 'true' : undefined"
            aria-describedby="project-rename-hint"
            class="field max-w-sm"
          />
          <button type="submit" :disabled="busy || newName.trim() === project.title" class="btn-primary">Save</button>
        </div>
        <p id="project-rename-hint" class="text-xs" :class="renameProblem ? 'text-danger' : 'text-ink-muted'">
          {{ renameProblem || `Its key, ${project.key}, stays: the editor finds the project by its key.` }}
        </p>
      </form>

      <!-- Its deletion, behind its key typed. -->
      <form v-if="deleting" id="project-delete-form" class="space-y-3 rounded-panel border border-danger/40 bg-panel p-4 sm:p-6" @submit.prevent="confirmDelete">
        <p class="text-sm font-semibold text-danger">
          Delete {{ project.title }}? Its history and files go for good, and the editor can no longer pull or push {{ project.key }}.
        </p>
        <label for="project-delete-confirm" class="block text-sm text-ink">
          Type <span class="font-mono font-semibold">{{ project.key }}</span> to confirm
        </label>
        <input id="project-delete-confirm" v-model="typed" type="text" autocomplete="off" spellcheck="false" class="field max-w-xs font-mono" />
        <div class="flex flex-wrap gap-2">
          <button type="submit" :disabled="busy || typed.trim() !== project.key" class="btn-danger">{{ busy ? 'Deleting…' : 'Delete now' }}</button>
          <button type="button" :disabled="busy" class="btn-secondary" @click="deleting = false">Cancel</button>
        </div>
      </form>

      <!-- Its history: the revisions pushed, newest first. -->
      <section class="space-y-3" aria-labelledby="project-history">
        <h2 id="project-history" class="text-xl font-semibold text-ink">History</h2>
        <p v-if="project.head === 0" class="text-sm text-ink-muted">
          Nothing pushed yet. In the OpenNova Editor, put your project on NovaWorld as {{ project.key }}, then push to it.
        </p>
        <template v-else>
          <p v-if="history.revisions.length === 0" class="text-sm text-ink-muted">{{ history.loading ? 'Loading…' : 'The history could not be read.' }}</p>
          <ol v-else class="divide-y divide-border rounded-panel border border-border bg-panel">
            <li v-for="r in history.revisions" :key="r.number" class="project-revision flex flex-wrap items-baseline gap-x-3 gap-y-1 px-4 py-2 text-sm">
              <span class="font-mono text-xs font-semibold">r{{ r.number }}</span>
              <span class="min-w-0 flex-1 break-words text-ink">{{ r.message }}</span>
              <span class="text-xs text-ink-muted">
                {{ r.author }}, {{ r.file_count }} file{{ r.file_count === 1 ? '' : 's' }}, {{ formatMemory(r.bytes) }},
                <RelativeTime :value="r.created_at" :now="now" />
              </span>
            </li>
          </ol>
          <button v-if="history.more" type="button" :disabled="history.loading" class="btn-secondary" @click="olderRevisions">
            {{ history.loading ? 'Loading…' : 'Older revisions' }}
          </button>
        </template>
      </section>
    </template>
  </div>
</template>

<script setup lang="ts">
// /me/projects/<key>: one of the account's projects (found in GET
// /api/me/projects, which also names the games a mod is of): what it is, who
// gets it, its newest revision and the bytes it stores; its visibility and its
// name changed (PATCH /api/products/<key>) and
// its deletion behind its key typed (DELETE); and its history
// (GET /api/products/<key>/revisions), a page at a time. Nothing here changes
// by itself but a push from the editor, so the page reads again when its tab
// is shown.
import { computed, ref, watch } from 'vue';
import { RouterLink, useRoute, useRouter } from 'vue-router';
import RelativeTime from '../../components/RelativeTime.vue';
import { REVISIONS_PAGE, deleteProject, fetchMyProjects, fetchRevisions, updateProject, type ProjectError } from '../../api/products';
import { usePolling } from '../../composables/usePolling';
import { useSession } from '../../composables/useSession';
import { useUnsavedChanges } from '../../composables/useUnsavedChanges';
import type { MyProjects, Project, ProjectRevision } from '../../types/products';
import { formatMemory } from '../../utils/format';
import { NAME_MAX, nameProblem, projectKindLine, visibilityLine } from '../../utils/projectsView';

const labelClass = 'block text-xs font-medium uppercase tracking-wider text-ink-muted';

const route = useRoute();
const router = useRouter();
const { refresh: refreshSession } = useSession();
const projectKey = computed(() => String(route.params.key ?? ''));

// The account's projects and the games a mod may be for; null until the first
// read answered.
const mine = ref<MyProjects | null>(null);
const loadFailed = ref(false);
const busy = ref(false);
const now = ref(Date.now());
const message = ref('');
const error = ref('');

const project = computed(() => mine.value?.projects.find((p) => p.key === projectKey.value) ?? null);
const missing = computed(() => mine.value !== null && project.value === null);

function flash(text: string) {
  message.value = text;
  error.value = '';
}

function fail(text: string) {
  error.value = text;
  message.value = '';
}

async function onSessionEnded() {
  await refreshSession();
  await router.replace({ path: '/login', query: { redirect: route.fullPath } });
}

// A refusal of a change: the session gone sends to the login, else its words.
function refused(e: ProjectError) {
  if (e.code === 'unauthorized') void onSessionEnded();
  else fail(e.message);
}

usePolling({
  async read(signal) {
    const result = await fetchMyProjects(signal);
    if (signal.aborted) return false;
    now.value = Date.now();
    if (!result.ok) {
      if (result.error.code === 'unauthorized') await onSessionEnded();
      else loadFailed.value = true;
      return false;
    }
    mine.value = result.value;
    loadFailed.value = false;
    return true;
  },
  next: () => null,
});

// The project as a change answered it, in place until the next read.
function replace(changed: Project) {
  if (mine.value) mine.value = { ...mine.value, projects: mine.value.projects.map((p) => (p.key === changed.key ? changed : p)) };
}

// ---------------------------------------------------------------------------
// The history: its first page once the project has a revision, the rest when
// asked; read again when the head moves.

const history = ref<{ key: string; head: number; revisions: ProjectRevision[]; more: boolean; loading: boolean }>({
  key: '',
  head: 0,
  revisions: [],
  more: false,
  loading: false,
});

async function readHistory(before?: number) {
  const shown = history.value;
  shown.loading = true;
  const result = await fetchRevisions(shown.key, before);
  if (history.value !== shown) return;
  shown.loading = false;
  if (!result.ok) {
    fail(result.error.message);
    return;
  }
  shown.revisions = before ? [...shown.revisions, ...result.value] : result.value;
  const last = shown.revisions[shown.revisions.length - 1];
  shown.more = result.value.length === REVISIONS_PAGE && last !== undefined && last.number > 1;
}

watch(
  () => [project.value?.key ?? null, project.value?.head ?? 0] as const,
  ([key, head]) => {
    if (key === null || head === 0) return;
    if (history.value.key === key && history.value.head === head) return;
    history.value = { key, head, revisions: [], more: false, loading: true };
    void readHistory();
  },
  { immediate: true },
);

function olderRevisions() {
  const last = history.value.revisions[history.value.revisions.length - 1];
  if (last) void readHistory(last.number);
}

// Another project's page in the same view starts with every form closed.
watch(projectKey, () => {
  renaming.value = false;
  deleting.value = false;
  message.value = '';
  error.value = '';
});

// ---------------------------------------------------------------------------
// Who gets it, its name, its deletion.

async function toggleVisibility() {
  const p = project.value;
  if (!p || busy.value) return;
  const visibility = p.visibility === 'public' ? 'private' : 'public';
  busy.value = true;
  const result = await updateProject(p.key, { visibility });
  busy.value = false;
  if (!result.ok) {
    refused(result.error);
    return;
  }
  replace(result.value);
  flash(visibility === 'public' ? `Anyone may now get ${p.title}.` : `Only you and NovaWorld's admins may now get ${p.title}.`);
}

const renaming = ref(false);
const newName = ref('');
const renameTried = ref(false);
const renameProblem = computed(() => (renameTried.value ? nameProblem(newName.value) : ''));

useUnsavedChanges(() => renaming.value && !!project.value && newName.value.trim() !== project.value.title);

function toggleRename() {
  renaming.value = !renaming.value;
  deleting.value = false;
  newName.value = project.value?.title ?? '';
  renameTried.value = false;
}

async function saveRename() {
  const p = project.value;
  if (!p || busy.value) return;
  renameTried.value = true;
  const displayName = newName.value.trim();
  if (nameProblem(displayName) || displayName === p.title) return;
  busy.value = true;
  const result = await updateProject(p.key, { display_name: displayName });
  busy.value = false;
  if (!result.ok) {
    refused(result.error);
    return;
  }
  renaming.value = false;
  replace(result.value);
  flash(`${p.title} is now ${result.value.title}.`);
}

const deleting = ref(false);
const typed = ref('');

function toggleDelete() {
  renaming.value = false;
  deleting.value = !deleting.value;
  typed.value = '';
}

async function confirmDelete() {
  const p = project.value;
  if (!p || busy.value || typed.value.trim() !== p.key) return;
  busy.value = true;
  const result = await deleteProject(p.key);
  busy.value = false;
  if (!result.ok) {
    refused(result.error);
    return;
  }
  deleting.value = false;
  await router.push('/me/projects');
}
</script>
