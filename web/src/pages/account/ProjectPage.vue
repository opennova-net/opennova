<template>
  <div class="space-y-6">
    <PageHeader :title="project?.title ?? (missing ? 'No such project' : 'Project')" :crumbs="CRUMBS" :tabs="tabs">
      <template v-if="project" #meta>
        <span class="font-mono text-ink" title="Key">{{ project.key }}</span>
        <span id="project-kind-line">{{ projectKindLine(project, mine?.games ?? []) }}</span>
        <span v-if="project.protocol" id="project-protocol" title="The protocol its title speaks">{{ protocolWords(project.protocol) }}</span>
        <span v-if="project.kind === 'expansion' && project.expansion_code" class="font-mono" title="Short name">{{ project.expansion_code }}</span>
        <Badge id="project-visibility-badge" :tone="project.visibility === 'public' ? 'ok' : 'muted'" :title="visibilityLine(project.visibility)">
          {{ project.visibility === 'public' ? 'Public' : 'Private' }}
        </Badge>
        <span id="project-head" title="Newest revision">{{ project.head > 0 ? `r${project.head}` : 'Nothing pushed yet' }}</span>
        <span v-if="project.storage" title="Stored">{{ formatMemory(project.storage.bytes) }}</span>
        <span v-if="project.release" id="project-published-line">Players get {{ project.release.version ? releaseVersionWords(project.release.version, project.release.one_version) : `release ${project.release.id}` }}</span>
      </template>
      <template v-if="project?.can_manage" #actions>
        <button
          id="project-visibility"
          type="button"
          class="btn-secondary"
          :disabled="busy"
          :title="visibilityLine(project.visibility === 'public' ? 'private' : 'public')"
          @click="toggleVisibility"
        >
          {{ project.visibility === 'public' ? 'Make private' : 'Make public' }}
        </button>
        <button id="project-rename-toggle" type="button" class="btn-secondary" :disabled="busy" :aria-expanded="renaming" @click="toggleRename">Rename</button>
        <button id="project-delete-toggle" type="button" class="btn-danger" :disabled="busy" :aria-expanded="deleting" @click="toggleDelete">Delete</button>
      </template>
    </PageHeader>

    <LiveNotices :message="message" :error="error" :poll-trouble="pollTrouble" />

    <p v-if="missing" class="text-sm text-ink-muted">No project of yours has the key {{ projectKey }}.</p>
    <p v-else-if="!project" class="text-sm text-ink-muted">{{ loadFailed ? 'Could not read the project; retrying.' : 'Loading…' }}</p>

    <template v-else>
      <section v-if="project.can_manage" class="card max-w-xl space-y-2 p-4" aria-label="Asset sharing">
        <label class="flex items-center gap-2 text-sm text-ink">
          <input id="project-assets-public" type="checkbox" :checked="project.assets_public" :disabled="busy"
            aria-describedby="project-assets-hint" class="accent-accent" @change="toggleAssetsPublic" />
          Publish assets in library
        </label>
        <p id="project-assets-hint" class="text-xs text-ink-muted">
          Share native assets and their dependencies after a new release is built and published. Project visibility is a separate setting.
          Assets appear only when all required dependencies are also shared.
        </p>
      </section>

      <section id="project-lobby-settings" class="card max-w-xl space-y-3 p-4" aria-label="Lobby">
        <div class="flex flex-wrap items-center justify-between gap-2">
          <p class="text-sm text-ink">Lobby: <span id="project-lobby-current">{{ project.lobby?.display_name ?? 'Not configured' }}</span></p>
          <button v-if="project.kind === 'game' && project.can_manage && !project.lobby_locked && !editingLobby"
            id="project-lobby-edit" type="button" class="btn-secondary btn-sm" :disabled="busy" @click="beginLobbyEdit">Change lobby</button>
        </div>
        <p v-if="project.kind === 'expansion'" class="text-xs text-ink-muted">Uses its base game’s lobby.</p>
        <p v-else-if="project.lobby_locked" id="project-lobby-locked" class="text-xs text-ink-muted">The lobby is locked because a release build has started.</p>
        <form v-if="editingLobby && !project.lobby_locked" id="project-lobby-form" class="space-y-3" novalidate @submit.prevent="saveLobby">
          <ProjectLobbyPicker prefix="project" v-model:mode="lobbyMode" v-model:new-name="lobbyName" v-model:slug="lobbySlug"
            :options="lobbies" :disabled="busy" :error="lobbyProblem" />
          <div class="flex gap-2">
            <button type="submit" class="btn-primary" :disabled="busy">Save lobby</button>
            <button type="button" class="btn-ghost" :disabled="busy" @click="editingLobby = false">Cancel</button>
          </div>
        </form>
      </section>

      <form v-if="renaming" id="project-rename-form" class="card max-w-xl space-y-1 p-4" novalidate @submit.prevent="saveRename">
        <label for="project-rename-name" class="field-label">Name</label>
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
            class="field w-auto min-w-0 flex-1"
          />
          <button type="submit" :disabled="busy || newName.trim() === project.title" class="btn-primary">Save</button>
          <button type="button" :disabled="busy" class="btn-ghost" @click="renaming = false">Cancel</button>
        </div>
        <p id="project-rename-hint" class="text-xs text-danger empty:hidden">{{ renameProblem }}</p>
      </form>

      <form v-if="deleting" id="project-delete-form" class="card max-w-xl space-y-2 border-danger/40 p-4" @submit.prevent="confirmDelete">
        <label for="project-delete-confirm" class="block text-sm text-ink">
          Type <span class="font-mono font-semibold">{{ project.key }}</span> to delete {{ project.title }} and its history for good.
        </label>
        <div class="flex flex-wrap items-center gap-2">
          <input id="project-delete-confirm" v-model="typed" type="text" autocomplete="off" spellcheck="false" class="field w-auto font-mono" />
          <button type="submit" :disabled="busy || typed.trim() !== project.key" class="btn-danger">{{ busy ? 'Deleting…' : 'Delete' }}</button>
          <button type="button" :disabled="busy" class="btn-ghost" @click="deleting = false">Cancel</button>
        </div>
      </form>

      <section v-if="tab === 'releases'" id="project-releases" class="space-y-3" aria-label="Releases">
        <div class="flex flex-wrap items-center justify-between gap-3">
          <span id="project-release-usage" class="text-sm text-ink-muted" :title="`${formatMemory(mine!.usage.release_bytes)} of it is release packages`">
            {{ formatMemory(mine!.usage.bytes) }} of {{ formatMemory(mine!.limits.bytes) }} stored
          </span>
          <button v-if="project.head > 0 && !building" id="project-build-toggle" type="button" class="btn-secondary btn-sm" :disabled="busy" @click="beginBuild">
            Build a release
          </button>
        </div>

        <form v-if="building" id="project-build-form" class="card flex flex-wrap items-end gap-3 p-4" novalidate @submit.prevent="confirmBuild">
          <div>
            <label for="project-build-version" class="field-label">Game version</label>
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
          <div>
            <label for="project-build-revision" class="field-label">Revision</label>
            <input id="project-build-revision" v-model.number="buildRevision" type="number" min="1" :max="project.head" :disabled="busy" class="field w-24 font-mono" />
          </div>
          <button type="submit" :disabled="busy || !buildVersion.trim()" class="btn-primary">{{ busy ? 'Starting…' : 'Build' }}</button>
          <button type="button" :disabled="busy" class="btn-ghost" @click="building = false">Cancel</button>
          <p id="project-build-hint" class="w-full text-xs text-danger empty:hidden">{{ versionProblem }}</p>
        </form>

        <form v-if="publishing" id="project-publish-form" class="card space-y-3 p-4" @submit.prevent="confirmPublish">
          <p class="text-sm font-semibold text-ink">
            {{ needsApproval ? `Submit ${releaseLabel(publishing)} for an admin's approval?` : `Publish ${releaseLabel(publishing)} to players?` }}
          </p>
          <label v-if="project.visibility === 'private'" class="flex items-center gap-2 text-sm text-ink" title="Private means unlisted, not secret.">
            <input id="project-publish-public" v-model="makePublic" type="checkbox" :disabled="busy" class="accent-accent" />
            Make {{ project.title }} public too
          </label>
          <div class="flex flex-wrap gap-2">
            <button id="project-publish-confirm" type="submit" :disabled="busy" class="btn-primary">
              {{ needsApproval ? (busy ? 'Submitting…' : 'Submit for approval') : busy ? 'Publishing…' : 'Publish' }}
            </button>
            <button type="button" :disabled="busy" class="btn-ghost" @click="publishing = null">Cancel</button>
          </div>
        </form>

        <div class="table-wrap">
          <table class="data-table">
            <thead>
              <tr>
                <th scope="col">Game version</th>
                <th scope="col">Status</th>
                <th scope="col">From</th>
                <th scope="col">Made</th>
                <th scope="col">Size</th>
                <th scope="col"><span class="sr-only">Actions</span></th>
              </tr>
            </thead>
            <tbody>
              <TableState
                v-if="shownReleases.length === 0"
                :colspan="6"
                :rows="releasesRead ? shownReleases : null"
                :loading="!releasesTrouble"
                empty="No releases yet."
              />
              <tr v-for="r in shownReleases" :key="r.id" class="project-release">
                <th scope="row" class="font-mono">
                  <span class="release-version">{{ releaseLabel(r) }}</span>
                  <span v-if="r.one_version" class="release-one block font-sans text-xs font-normal text-ink-muted">ONE {{ r.one_version }}</span>
                </th>
                <td class="space-y-1">
                  <span :class="releaseStatusClass(r.status)" :title="r.status === 'awaiting_approval' ? 'Players get it once an admin approves it.' : undefined">
                    {{ releaseStatusLabel(r.status) }}
                  </span>
                  <p v-if="treeOf(r)" class="text-xs text-ink-muted">{{ treeStateText(treeOf(r)!) }}</p>
                  <p v-if="r.status === 'draft' && r.review_note" class="project-release-note break-words text-xs text-warn">Sent back: {{ r.review_note }}</p>
                  <p v-if="r.error" class="release-error break-words text-xs text-danger">{{ r.error }}</p>
                  <details v-if="r.build_log" class="project-release-log text-xs" :open="r.status === 'building'">
                    <summary class="text-accent hover:text-ink-bright">Build log</summary>
                    <pre class="mt-1 max-h-64 max-w-xl overflow-auto whitespace-pre-wrap break-all rounded-control border border-border bg-surface p-2 font-mono text-ink">{{ r.build_log }}</pre>
                  </details>
                </td>
                <td class="whitespace-nowrap font-mono text-xs">
                  <template v-if="r.source">r{{ r.source.revision }}<span v-if="r.source.base_revision" class="text-ink-muted" title="Its game's revision"> on r{{ r.source.base_revision }}</span></template>
                </td>
                <td class="text-xs text-ink-muted">
                  <RelativeTime :value="r.created_at" :now="now" /> {{ creatorLabel(r.created_by) }}
                  <span v-if="r.published_at" class="block">published <RelativeTime :value="r.published_at" :now="now" /></span>
                </td>
                <td class="whitespace-nowrap">{{ r.bytes > 0 ? formatMemory(r.bytes) : '' }}</td>
                <td class="whitespace-nowrap text-right">
                  <button v-if="r.status === 'draft'" type="button" :disabled="busy" class="btn-ghost btn-sm" @click="beginPublish(r)">
                    {{ needsApproval ? 'Submit for approval' : 'Publish' }}<span class="sr-only"> release {{ releaseLabel(r) }}</span>
                  </button>
                  <button v-if="r.status === 'awaiting_approval'" type="button" :disabled="busy" class="btn-danger btn-sm" @click="removeRelease(r)">
                    Withdraw<span class="sr-only"> release {{ releaseLabel(r) }}</span>
                  </button>
                  <button v-if="r.status === 'published'" type="button" :disabled="busy" class="btn-warn btn-sm" @click="retire(r)">
                    Retire<span class="sr-only"> release {{ releaseLabel(r) }}</span>
                  </button>
                  <button
                    v-if="r.status === 'draft' || r.status === 'invalid' || r.status === 'retired'"
                    type="button"
                    :disabled="busy"
                    class="btn-danger btn-sm ml-2"
                    @click="removeRelease(r)"
                  >
                    Delete<span class="sr-only"> release {{ releaseLabel(r) }}</span>
                  </button>
                </td>
              </tr>
            </tbody>
          </table>
        </div>

      </section>

      <section v-else id="project-history" class="space-y-3" aria-label="History">
        <div class="table-wrap">
          <table class="data-table">
            <thead>
              <tr>
                <th scope="col">Revision</th>
                <th scope="col">Message</th>
                <th scope="col">By</th>
                <th scope="col">Files</th>
                <th scope="col">Size</th>
                <th scope="col">When</th>
              </tr>
            </thead>
            <tbody>
              <TableState
                v-if="project.head === 0 || history.revisions.length === 0"
                :colspan="6"
                :rows="project.head === 0 ? [] : null"
                :loading="history.loading"
                empty="Nothing pushed yet."
              />
              <template v-else>
                <tr v-for="r in history.revisions" :key="r.number" class="project-revision">
                  <th scope="row" class="font-mono text-xs">r{{ r.number }}</th>
                  <td class="break-words">{{ r.message }}</td>
                  <td>{{ r.author }}</td>
                  <td>{{ r.file_count }}</td>
                  <td class="whitespace-nowrap">{{ formatMemory(r.bytes) }}</td>
                  <td class="whitespace-nowrap text-ink-muted"><RelativeTime :value="r.created_at" :now="now" /></td>
                </tr>
              </template>
            </tbody>
          </table>
        </div>
        <button v-if="history.more" type="button" :disabled="history.loading" class="btn-secondary btn-sm" @click="olderRevisions">
          {{ history.loading ? 'Loading…' : 'Older revisions' }}
        </button>
      </section>
    </template>
  </div>
</template>

<script setup lang="ts">
// /me/projects/<key>: one of the account's projects (found in GET
// /api/me/projects): what it is in the header's facts; its visibility and
// name (PATCH /api/products/<key>) and its deletion behind its key typed
// (DELETE); then two tabs. Releases (GET /api/products/<key>/releases), built
// from a revision, then published (a player's submitted for an admin's
// approval, which it may withdraw; a private project's publish may make it
// public), retired or deleted; History (GET /api/products/<key>/revisions), a
// page at a time. It polls while a release builds or its servers' data does;
// otherwise it reads again when the browser tab is shown.
import { computed, ref, watch } from 'vue';
import { useRoute, useRouter } from 'vue-router';
import RelativeTime from '../../components/RelativeTime.vue';
import Badge from '../../components/ui/Badge.vue';
import LiveNotices from '../../components/ui/LiveNotices.vue';
import PageHeader from '../../components/ui/PageHeader.vue';
import TableState from '../../components/ui/TableState.vue';
import type { Tab } from '../../components/ui/TabNav.vue';
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
import ProjectLobbyPicker from '../../components/ProjectLobbyPicker.vue';
import type { Crumb } from '../../utils/adminNav';
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
import { NAME_MAX, VERSION_HINT, myProjectLink, nameProblem, projectKindLine, visibilityLine } from '../../utils/projectsView';
import { lobbiesForTitle, protocolWords, releaseVersionWords } from '../../utils/games';

const BUSY_POLL_MS = 3_000;
const CRUMBS: Crumb[] = [{ label: 'Projects', to: '/me/projects' }];

const route = useRoute();
const router = useRouter();
const { user, refresh: refreshSession } = useSession();
// A player's publish is a submission an admin approves; an admin's publishes.
const needsApproval = computed(() => user.value?.role !== 'admin');
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

// Its two parts, Releases (for a project it may release) and History, as tabs
// (?tab=history); one it may not release shows its history alone.
const tab = computed(() => (project.value?.can_release && route.query.tab !== 'history' ? 'releases' : 'history'));
const tabs = computed<Tab[]>(() => {
  if (!project.value?.can_release) return [];
  const path = myProjectLink(projectKey.value);
  return [
    { label: 'Releases', to: { path } },
    { label: 'History', to: { path, query: { tab: 'history' } } },
  ];
});

// The project's releases as last read, with the key they are of; null before
// the first read.
const releases = ref<{ key: string; list: Release[] } | null>(null);
const releasesTrouble = ref(false);
const releasesRead = computed(() => releases.value?.key === projectKey.value);
const shownReleases = computed(() => (releasesRead.value ? releases.value!.list : []));
// A background read failed while the page shows what it read before.
const pollTrouble = computed(() => (loadFailed.value && project.value !== null) || (releasesTrouble.value && releasesRead.value));

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
// The history: its first page once its tab shows a project with a revision,
// the rest when asked; read again when the head moves.

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
  () => [project.value?.key ?? null, project.value?.head ?? 0, tab.value] as const,
  ([key, head, shown]) => {
    if (key === null || head === 0 || shown !== 'history') return;
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
  editingLobby.value = false;
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

const editingLobby = ref(false);
const lobbyMode = ref<'new' | 'existing'>('existing');
const lobbyName = ref('');
const lobbySlug = ref('');
const lobbyProblem = ref('');
const lobbies = computed(() => {
  const p = project.value;
  // The lobbies of the protocol the project's title speaks.
  const options = lobbiesForTitle(mine.value?.lobbies ?? [], p?.game ?? '', mine.value?.protocols);
  // The selected lobby may no longer be offered for new projects.
  return p?.lobby && !options.some((lobby) => lobby.slug === p.lobby!.slug) ? [p.lobby, ...options] : options;
});

watch(() => project.value?.lobby_locked, (locked) => {
  if (locked) editingLobby.value = false;
});

function beginLobbyEdit() {
  lobbyMode.value = project.value?.lobby ? 'existing' : 'new';
  lobbyName.value = project.value?.title ?? '';
  lobbySlug.value = project.value?.lobby?.slug ?? '';
  lobbyProblem.value = '';
  editingLobby.value = true;
}

async function saveLobby() {
  const p = project.value;
  if (!p || !p.can_manage || p.kind !== 'game' || p.lobby_locked || busy.value) return;
  lobbyProblem.value = lobbyMode.value === 'new' ? nameProblem(lobbyName.value)
    : lobbies.value.some((lobby) => lobby.slug === lobbySlug.value) ? '' : 'Pick an available lobby for this game.';
  if (lobbyProblem.value) return;
  busy.value = true;
  const patch = lobbyMode.value === 'new' ? { new_lobby_name: lobbyName.value.trim() } : { lobby: lobbySlug.value };
  const result = await updateProject(p.key, patch);
  busy.value = false;
  if (!result.ok) {
    if (result.error.code === 'lobby_locked') refused(result.error);
    else if (result.error.field === 'lobby' || result.error.field === 'new_lobby_name') lobbyProblem.value = result.error.message;
    else refused(result.error);
    if (result.error.code === 'lobby_locked') await poll.readAgain();
    return;
  }
  editingLobby.value = false;
  replace(result.value);
  flash(`${p.title} now uses the ${result.value.lobby?.display_name ?? 'selected'} lobby.`);
}

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
  flash(`${p.title} is now ${visibility}.`);
}

async function toggleAssetsPublic(event: Event) {
  const input = event.target as HTMLInputElement;
  const p = project.value;
  if (!p || !p.can_manage || busy.value) return;
  const assets_public = input.checked;
  busy.value = true;
  const result = await updateProject(p.key, { assets_public });
  busy.value = false;
  if (!result.ok) {
    input.checked = p.assets_public;
    refused(result.error);
    return;
  }
  replace(result.value);
  flash(`Asset sharing ${assets_public ? 'enabled' : 'disabled'} for ${p.title}.`);
}

const renaming = ref(false);
const newName = ref('');
const renameTried = ref(false);
const renameProblem = computed(() => (renameTried.value ? nameProblem(newName.value) : ''));

useUnsavedChanges(
  () =>
    (renaming.value && !!project.value && newName.value.trim() !== project.value.title) ||
    (editingLobby.value && (lobbyMode.value === 'new' || lobbySlug.value !== project.value?.lobby?.slug)) ||
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
  flash(`${releaseLabel(result.value)} is building.`);
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
  const done = result.value.status === 'awaiting_approval' ? `${name} awaits an admin's approval` : `${name} is published`;
  flash(goPublic ? `${done}, and ${p.title} is now public.` : `${done}.`);
  await poll.readAgain();
}

async function retire(r: Release) {
  const p = project.value;
  if (!p || busy.value) return;
  const name = releaseLabel(r);
  if (!(await confirmAction(`Retire ${name}? Players stop getting it; running servers keep it.`))) return;
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

// A release deleted; one awaiting approval is withdrawn so.
async function removeRelease(r: Release) {
  const p = project.value;
  if (!p || busy.value) return;
  const name = releaseLabel(r);
  const withdraw = r.status === 'awaiting_approval';
  const ask = withdraw ? `Withdraw ${name}? It is deleted with its package.` : `Delete ${name}? Its package goes for good.`;
  if (!(await confirmAction(ask))) return;
  busy.value = true;
  const result = await deleteProjectRelease(p.key, r.id);
  busy.value = false;
  if (!result.ok) {
    releaseRefused(result.error);
    return;
  }
  if (publishing.value?.id === r.id) publishing.value = null;
  flash(withdraw ? `${name} is withdrawn.` : `${name} is deleted.`);
  await poll.readAgain();
}
</script>
