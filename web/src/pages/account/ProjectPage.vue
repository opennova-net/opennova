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

      <!-- Its releases: revisions packed and checked, then published to players
           and hosted servers. -->
      <section class="space-y-3" aria-labelledby="project-releases">
        <h2 id="project-releases" class="text-xl font-semibold text-ink">Releases</h2>
        <p class="max-w-3xl text-sm text-ink-muted">
          NovaWorld packs a revision and checks it, then keeps it as a draft. Publishing it serves it to players' updaters
          and lets hosted servers run it; retiring it stops the serving.
          <template v-if="project.kind === 'expansion'">Players install a published public mod from the NovaWorld panel in game.</template>
          <template v-else>A published game lists its servers in a lobby of its own and, while public, has a download on the Downloads page.</template>
        </p>
        <p id="project-published-line" class="text-sm text-ink">
          {{ project.release ? `Players update to ${project.release.version || `release ${project.release.id}`}.` : 'Nothing published yet.' }}
        </p>

        <p v-if="!project.can_release" class="text-sm text-ink-muted">Only its owner and NovaWorld's admins may release it.</p>
        <template v-else>
          <p v-if="!releasesRead" class="text-sm text-ink-muted">
            {{ releasesTrouble ? 'The releases could not be read. This page tries again by itself.' : 'Loading…' }}
          </p>
          <p v-else-if="shownReleases.length === 0" class="text-sm text-ink-muted">No release yet.</p>
          <ol v-else class="divide-y divide-border rounded-panel border border-border bg-panel">
            <li v-for="r in shownReleases" :key="r.id" class="project-release space-y-1 px-4 py-3 text-sm">
              <div class="flex flex-wrap items-baseline gap-x-3 gap-y-1">
                <span class="font-mono font-semibold text-ink">{{ releaseLabel(r) }}</span>
                <span :class="releaseStatusClass(r.status)">{{ releaseStatusLabel(r.status) }}</span>
                <span class="min-w-0 flex-1 text-xs text-ink-muted">
                  <template v-if="r.source">from r{{ r.source.revision }}<template v-if="r.source.base_revision"> on its game's r{{ r.source.base_revision }}</template>, </template>
                  <template v-if="r.bytes > 0">{{ formatMemory(r.bytes) }}, </template>
                  made <RelativeTime :value="r.created_at" :now="now" /> {{ creatorLabel(r.created_by) }}<template v-if="r.published_at">, published <RelativeTime :value="r.published_at" :now="now" /></template>
                </span>
                <span class="flex gap-3 text-xs">
                  <button v-if="r.status === 'draft'" type="button" :disabled="busy" class="text-accent hover:text-ink-bright disabled:opacity-40" @click="beginPublish(r)">
                    Publish<span class="sr-only"> release {{ releaseLabel(r) }}</span>
                  </button>
                  <button v-if="r.status === 'published'" type="button" :disabled="busy" class="text-warn/90 hover:text-warn disabled:opacity-40" @click="retire(r)">
                    Retire<span class="sr-only"> release {{ releaseLabel(r) }}</span>
                  </button>
                  <button
                    v-if="r.status === 'draft' || r.status === 'invalid' || r.status === 'retired'"
                    type="button"
                    :disabled="busy"
                    class="text-danger/80 hover:text-danger disabled:opacity-40"
                    @click="removeRelease(r)"
                  >
                    Delete<span class="sr-only"> release {{ releaseLabel(r) }}</span>
                  </button>
                </span>
              </div>
              <p v-if="r.error" class="break-words text-xs text-danger">{{ r.error }}</p>
              <p v-if="treeOf(r)" class="text-xs text-ink-muted">{{ treeStateText(treeOf(r)!) }}</p>
              <details v-if="r.build_log" class="project-release-log text-xs" :open="r.status === 'building'">
                <summary class="cursor-pointer text-accent hover:text-ink-bright">Build log</summary>
                <pre class="mt-1 max-h-64 overflow-auto whitespace-pre-wrap break-all rounded-control border border-border bg-surface p-2 font-mono text-ink">{{ r.build_log }}</pre>
              </details>
            </li>
          </ol>

          <!-- A draft's publishing; a private project's offers to make it public. -->
          <form v-if="publishing" id="project-publish-form" class="space-y-3 rounded-panel border border-border bg-panel p-4 sm:p-6" @submit.prevent="confirmPublish">
            <p class="text-sm font-semibold text-ink">Publish {{ releaseLabel(publishing) }} of {{ project.title }}?</p>
            <p class="text-sm text-ink-muted">
              Players' updaters get it, and new hosted servers run it once its data is built. Running servers keep what
              they run until their owners move them.
            </p>
            <template v-if="project.visibility === 'private'">
              <label class="flex items-start gap-3 text-sm text-ink">
                <input id="project-publish-public" v-model="makePublic" type="checkbox" :disabled="busy" class="mt-0.5 h-4 w-4 rounded border-border bg-surface text-accent focus:ring-accent" />
                <span>
                  Make {{ project.title }} public too, so other players find it
                  {{ project.kind === 'expansion' ? 'in game' : 'on the Downloads page' }} and can host it.
                </span>
              </label>
              <p class="text-xs text-ink-muted">
                {{ project.kind === 'expansion' ? 'Left private, only you find it in game and can host it.' : 'Left private, it has no download and only you can host it.' }}
                Private means unlisted, not secret: anyone who has its update files can still get it.
              </p>
            </template>
            <div class="flex flex-wrap gap-2">
              <button id="project-publish-confirm" type="submit" :disabled="busy" class="btn-primary">{{ busy ? 'Publishing…' : 'Publish now' }}</button>
              <button type="button" :disabled="busy" class="btn-secondary" @click="publishing = null">Cancel</button>
            </div>
          </form>

          <template v-if="project.head > 0">
            <button v-if="!building" id="project-build-toggle" type="button" class="btn-secondary" :disabled="busy" @click="beginBuild">Build a release…</button>
            <form v-else id="project-build-form" class="space-y-3 rounded-panel border border-border bg-panel p-4 sm:p-6" novalidate @submit.prevent="confirmBuild">
              <p class="text-sm text-ink">
                NovaWorld packs the revision whole<template v-if="project.kind === 'expansion'">, on its game's newest revision</template>,
                and checks it. It shows here as a draft once that is done; nothing reaches players until you publish it.
              </p>
              <div class="flex flex-wrap items-end gap-3">
                <div class="space-y-1">
                  <label for="project-build-version" :class="labelClass">Version</label>
                  <input
                    id="project-build-version"
                    v-model="buildVersion"
                    type="text"
                    autocomplete="off"
                    spellcheck="false"
                    placeholder="1.0"
                    maxlength="32"
                    :disabled="busy"
                    :aria-invalid="versionProblem ? 'true' : undefined"
                    aria-describedby="project-build-hint"
                    class="field w-32 font-mono"
                  />
                </div>
                <div class="space-y-1">
                  <label for="project-build-revision" :class="labelClass">Revision</label>
                  <input id="project-build-revision" v-model.number="buildRevision" type="number" min="1" :max="project.head" :disabled="busy" class="field w-24 font-mono" />
                </div>
                <button type="submit" :disabled="busy || !buildVersion.trim()" class="btn-primary">{{ busy ? 'Starting…' : 'Build release' }}</button>
                <button type="button" :disabled="busy" class="btn-secondary" @click="building = false">Cancel</button>
              </div>
              <p id="project-build-hint" class="text-xs" :class="versionProblem ? 'text-danger' : 'text-ink-muted'">
                {{ versionProblem || `${VERSION_HINT} r${project.head} is the newest revision.` }}
              </p>
            </form>
          </template>
          <p v-else class="text-sm text-ink-muted">Push a revision from the editor to build a release.</p>

          <p id="project-release-usage" class="text-xs text-ink-muted">
            Your account stores {{ formatMemory(mine!.usage.bytes) }} of the {{ formatMemory(mine!.limits.bytes) }} it may;
            {{ formatMemory(mine!.usage.release_bytes) }} of it is release packages. Deleting a release frees its package.
          </p>
        </template>
      </section>

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
// /api/me/projects, which also names the games a mod is of and what the
// account stores): what it is, who gets it, its newest revision and the bytes
// it stores; its visibility and its name changed (PATCH /api/products/<key>)
// and its deletion behind its key typed (DELETE); its releases (GET
// /api/products/<key>/releases), each built from a revision (POST), then
// published (a private project's publish offers to make it public, by the
// PATCH), retired or deleted; and its history (GET
// /api/products/<key>/revisions), a page at a time. The page polls while a
// release is being built or checked or its servers' data is building;
// otherwise nothing here changes by itself but a push from the editor, so it
// reads again when its tab is shown.
import { computed, ref, watch } from 'vue';
import { RouterLink, useRoute, useRouter } from 'vue-router';
import RelativeTime from '../../components/RelativeTime.vue';
import {
  REVISIONS_PAGE,
  buildRelease,
  deleteProject,
  deleteProjectRelease,
  fetchMyProjects,
  fetchProjectReleases,
  fetchRevisions,
  publishProjectRelease,
  retireProjectRelease,
  updateProject,
  type ProjectError,
} from '../../api/products';
import { confirmAction } from '../../composables/useConfirm';
import { usePolling } from '../../composables/usePolling';
import { useSession } from '../../composables/useSession';
import { useUnsavedChanges } from '../../composables/useUnsavedChanges';
import type { MyProjects, Project, ProjectRevision, Release } from '../../types/products';
import { formatMemory } from '../../utils/format';
import {
  VERSION_RULE,
  creatorLabel,
  releaseLabel,
  releaseStatusClass,
  releaseStatusLabel,
  releasesWorking,
  treeStateText,
} from '../../utils/productsView';
import { NAME_MAX, VERSION_HINT, nameProblem, projectKindLine, visibilityLine } from '../../utils/projectsView';

const BUSY_POLL_MS = 3_000;

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

// The project's releases as last read, with the key they are of; null before
// the first read.
const releases = ref<{ key: string; list: Release[] } | null>(null);
const releasesTrouble = ref(false);
const releasesRead = computed(() => releases.value?.key === projectKey.value);
const shownReleases = computed(() => (releasesRead.value ? releases.value!.list : []));

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

// The account's projects, then this one's releases when it may release it.
const poll = usePolling({
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
    const p = project.value;
    if (!p?.can_release) return true;
    const list = await fetchProjectReleases(p.key, signal);
    if (signal.aborted) return false;
    if (!list.ok) {
      if (list.error.code === 'unauthorized') await onSessionEnded();
      else releasesTrouble.value = true;
      return false;
    }
    releases.value = { key: p.key, list: list.value };
    releasesTrouble.value = false;
    return true;
  },
  next: () => (releasesWorking(shownReleases.value) ? BUSY_POLL_MS : null),
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

// Another project's page in the same view starts with every form closed, and
// reads its releases.
watch(projectKey, (key) => {
  renaming.value = false;
  deleting.value = false;
  building.value = false;
  publishing.value = null;
  message.value = '';
  error.value = '';
  if (key) void poll.readAgain();
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

useUnsavedChanges(
  () =>
    (renaming.value && !!project.value && newName.value.trim() !== project.value.title) ||
    (building.value && buildVersion.value.trim() !== ''),
);

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

// ---------------------------------------------------------------------------
// Its releases.

// The servers' data a published release built for this project, by its state.
function treeOf(r: Release): string | null {
  if (r.status !== 'published') return null;
  return r.sections.find((s) => s.product_id === project.value?.product_id)?.tree?.state ?? null;
}

// A refusal of a release action, said; the list read again, as it may have
// moved on.
function releaseRefused(e: ProjectError) {
  if (e.code === 'not_found') fail('That release is no longer on NovaWorld.');
  else refused(e);
  if (e.code !== 'unauthorized') void poll.readAgain();
}

const building = ref(false);
const buildVersion = ref('');
const buildRevision = ref(0);
const buildTried = ref(false);
const versionProblem = computed(() => (buildTried.value && !VERSION_RULE.test(buildVersion.value.trim()) ? VERSION_HINT : ''));

function beginBuild() {
  building.value = true;
  publishing.value = null;
  buildVersion.value = '';
  buildRevision.value = project.value?.head ?? 0;
  buildTried.value = false;
}

async function confirmBuild() {
  const p = project.value;
  if (!p || busy.value) return;
  buildTried.value = true;
  const version = buildVersion.value.trim();
  if (!VERSION_RULE.test(version)) return;
  busy.value = true;
  const result = await buildRelease(p.key, { version, revision: buildRevision.value || p.head });
  busy.value = false;
  if (!result.ok) {
    releaseRefused(result.error);
    return;
  }
  building.value = false;
  flash(`${releaseLabel(result.value)} is building. It shows here as a draft to publish once NovaWorld has checked it.`);
  await poll.readAgain();
}

const publishing = ref<Release | null>(null);
const makePublic = ref(true);

function beginPublish(r: Release) {
  publishing.value = r;
  makePublic.value = true;
  building.value = false;
}

// The publish, after making the project public when asked: what is public by
// the time the release is served is listed at once.
async function confirmPublish() {
  const p = project.value;
  const r = publishing.value;
  if (!p || !r || busy.value) return;
  const name = releaseLabel(r);
  const goPublic = p.visibility === 'private' && makePublic.value;
  busy.value = true;
  if (goPublic) {
    const shown = await updateProject(p.key, { visibility: 'public' });
    if (!shown.ok) {
      busy.value = false;
      refused(shown.error);
      return;
    }
    replace(shown.value);
  }
  const result = await publishProjectRelease(p.key, r.id);
  busy.value = false;
  if (!result.ok) {
    releaseRefused(result.error);
    return;
  }
  publishing.value = null;
  flash(goPublic ? `${name} is published, and anyone may now get ${p.title}.` : `${name} is published.`);
  await poll.readAgain();
}

async function retire(r: Release) {
  const p = project.value;
  if (!p || busy.value) return;
  const name = releaseLabel(r);
  if (!(await confirmAction(`Retire ${name}? The update service stops serving it; servers keep what they run.`))) return;
  busy.value = true;
  const result = await retireProjectRelease(p.key, r.id);
  busy.value = false;
  if (!result.ok) {
    releaseRefused(result.error);
    return;
  }
  flash(`${name} is retired.`);
  await poll.readAgain();
}

async function removeRelease(r: Release) {
  const p = project.value;
  if (!p || busy.value) return;
  const name = releaseLabel(r);
  if (!(await confirmAction(`Delete ${name}? Its package goes for good, and stops counting against what your account may store.`))) return;
  busy.value = true;
  const result = await deleteProjectRelease(p.key, r.id);
  busy.value = false;
  if (!result.ok) {
    releaseRefused(result.error);
    return;
  }
  if (publishing.value?.id === r.id) publishing.value = null;
  flash(`${name} is deleted.`);
  await poll.readAgain();
}
</script>
