<template>
  <div class="space-y-10">
    <AdminPageHeader
      title="Content"
      description="The OpenNova Editor's projects, their files kept by revision. Editors push and pull them; players get only what is released."
      :loaded-at="loadedAt"
      :now="now"
      :loading="poll.loading.value"
      @refresh="poll.refresh"
    />

    <div role="status" :class="message ? 'rounded-panel border border-online/40 bg-online/10 p-4 text-sm text-online' : 'sr-only'">
      {{ message }}
    </div>
    <div role="alert" :class="error ? 'rounded-panel border border-danger/40 bg-danger/10 p-4 text-sm text-danger' : 'sr-only'">
      {{ error }}
    </div>
    <p v-if="pollTrouble" class="text-xs text-warn">The last refresh did not get through; this page keeps trying.</p>

    <section class="space-y-4" aria-labelledby="content-projects">
      <h2 id="content-projects" class="text-xl font-semibold text-ink">Projects</h2>
      <div class="relative overflow-x-auto rounded-panel border border-border bg-panel">
        <table class="min-w-full divide-y divide-border">
          <caption class="sr-only">Content projects</caption>
          <thead>
            <tr class="bg-raised text-left text-xs font-semibold uppercase tracking-wider text-ink-muted">
              <th scope="col" class="px-4 py-3">Project</th>
              <th scope="col" class="px-4 py-3">Who reads it</th>
              <th scope="col" class="px-4 py-3">Newest</th>
              <th scope="col" class="px-4 py-3">Builds on</th>
              <th scope="col" class="px-4 py-3">Updates</th>
              <th scope="col" class="px-4 py-3">Stored</th>
              <th scope="col" class="px-4 py-3 text-right">Actions</th>
            </tr>
          </thead>
          <tbody class="divide-y divide-border text-sm text-ink">
            <tr v-if="projects === null">
              <td colspan="7" class="px-4 py-6 text-center text-ink-muted">{{ poll.loading.value ? 'Loading…' : 'Not loaded.' }}</td>
            </tr>
            <tr v-else-if="projects.length === 0">
              <td colspan="7" class="px-4 py-6 text-center text-ink-muted">No content project yet.</td>
            </tr>
            <template v-for="p in projects ?? []" :key="p.key">
              <tr class="content-row align-top hover:bg-hover">
                <th scope="row" class="px-4 py-3 text-left font-medium">
                  {{ p.title }}
                  <span class="block font-mono text-xs font-normal text-ink-muted">
                    {{ p.key }}<template v-if="p.retail">, a retail import</template>
                  </span>
                </th>
                <td class="whitespace-nowrap px-4 py-3">
                  {{ p.visibility === 'public' ? 'Anyone' : 'Admins only' }}
                </td>
                <td class="whitespace-nowrap px-4 py-3">{{ p.head > 0 ? `r${p.head}` : 'Nothing pushed' }}</td>
                <td class="whitespace-nowrap px-4 py-3 font-mono text-xs">{{ p.base ?? '' }}<span v-if="!p.base" class="font-sans text-ink-muted">Nothing</span></td>
                <td class="px-4 py-3">{{ dataSetName(p.data_set_id) }}</td>
                <td class="whitespace-nowrap px-4 py-3">
                  <template v-if="p.storage">{{ p.storage.files }} file{{ p.storage.files === 1 ? '' : 's' }}, {{ formatBytes(p.storage.bytes) }}</template>
                </td>
                <td class="space-x-3 whitespace-nowrap px-4 py-3 text-right">
                  <button type="button" :disabled="busy" class="text-accent hover:text-ink-bright disabled:opacity-40" @click="toggleHistory(p)">
                    {{ history?.key === p.key ? 'Hide history' : 'History' }}<span class="sr-only"> of {{ p.key }}</span>
                  </button>
                  <button
                    v-if="p.data_set_id !== null && p.head > 0"
                    type="button"
                    :disabled="busy"
                    class="text-accent hover:text-ink-bright disabled:opacity-40"
                    @click="beginRelease(p)"
                  >
                    Release…<span class="sr-only"> {{ p.key }}</span>
                  </button>
                  <button type="button" :disabled="busy" class="text-ink hover:text-ink-bright disabled:opacity-40" @click="toggleVisibility(p)">
                    {{ p.visibility === 'public' ? 'Make private' : 'Make public' }}<span class="sr-only"> {{ p.key }}</span>
                  </button>
                  <button type="button" :disabled="busy" class="text-ink hover:text-ink-bright disabled:opacity-40" @click="beginRename(p)">
                    Rename<span class="sr-only"> {{ p.key }}</span>
                  </button>
                  <button type="button" :disabled="busy" class="text-danger/80 hover:text-danger disabled:opacity-40" @click="beginDelete(p)">
                    Delete…<span class="sr-only"> {{ p.key }}</span>
                  </button>
                </td>
              </tr>
              <tr v-if="renaming === p.key">
                <td colspan="7" class="px-4 py-3">
                  <form class="space-y-2" novalidate @submit.prevent="saveRename(p)">
                    <label :for="`content-rename-${p.key}`" class="block text-xs font-medium uppercase tracking-wider text-ink-muted">
                      New title for {{ p.key }}
                    </label>
                    <div class="flex flex-wrap items-center gap-2">
                      <input
                        :id="`content-rename-${p.key}`"
                        v-model="newTitle"
                        type="text"
                        autocomplete="off"
                        :maxlength="TITLE_MAX"
                        :class="fieldClass"
                      />
                      <button type="submit" :disabled="busy || !newTitle.trim() || newTitle.trim() === p.title" :class="primaryButton">
                        Save title
                      </button>
                      <button type="button" :disabled="busy" :class="secondaryButton" @click="renaming = null">Cancel</button>
                    </div>
                  </form>
                </td>
              </tr>
              <tr v-if="releasing === p.key">
                <td colspan="7" class="px-4 py-3">
                  <form class="space-y-2" novalidate @submit.prevent="confirmRelease(p)">
                    <p class="text-sm text-ink">
                      The data program builds a release of {{ p.title }} from a revision: a draft on Game data's Releases
                      once it is packed and checked, for you to publish there.
                    </p>
                    <div class="flex flex-wrap items-end gap-3">
                      <div class="space-y-1">
                        <label :for="`content-release-version-${p.key}`" class="block text-xs font-medium uppercase tracking-wider text-ink-muted">
                          Version
                        </label>
                        <input
                          :id="`content-release-version-${p.key}`"
                          v-model="releaseVersion"
                          type="text"
                          autocomplete="off"
                          spellcheck="false"
                          placeholder="0.2.0"
                          maxlength="32"
                          :aria-invalid="versionProblem ? 'true' : undefined"
                          class="w-32 rounded-control border border-border bg-surface px-3 py-2 font-mono text-sm text-ink focus:border-accent focus:outline-none aria-[invalid=true]:border-danger"
                        />
                      </div>
                      <div class="space-y-1">
                        <label :for="`content-release-revision-${p.key}`" class="block text-xs font-medium uppercase tracking-wider text-ink-muted">
                          Revision
                        </label>
                        <input
                          :id="`content-release-revision-${p.key}`"
                          v-model.number="releaseRevision"
                          type="number"
                          min="1"
                          :max="p.head"
                          class="w-24 rounded-control border border-border bg-surface px-3 py-2 font-mono text-sm text-ink focus:border-accent focus:outline-none"
                        />
                      </div>
                      <button type="submit" :disabled="busy || !releaseVersion.trim()" :class="primaryButton">
                        {{ busy ? 'Starting…' : 'Build release' }}
                      </button>
                      <button type="button" :disabled="busy" :class="secondaryButton" @click="releasing = null">Cancel</button>
                    </div>
                    <p class="text-xs" :class="versionProblem ? 'text-danger' : 'text-ink-muted'">
                      {{ versionProblem || `One to four numbers between dots. Revision ${p.head} is the newest.` }}
                    </p>
                  </form>
                </td>
              </tr>
              <tr v-if="deleting === p.key">
                <td colspan="7" class="px-4 py-3">
                  <form class="space-y-3" @submit.prevent="confirmDelete(p)">
                    <p class="text-sm font-semibold text-danger">
                      Delete {{ p.title }}? Its history and every file the store holds for it go, and an editor
                      bound to it can no longer pull or push. The store keeps the files 30 days, for a restore by hand.
                    </p>
                    <label :for="`content-delete-${p.key}`" class="block text-sm text-ink">
                      Type <span class="font-mono font-semibold">{{ p.key }}</span> to confirm
                    </label>
                    <input
                      :id="`content-delete-${p.key}`"
                      v-model="typedKey"
                      type="text"
                      autocomplete="off"
                      autocapitalize="characters"
                      spellcheck="false"
                      class="w-full max-w-xs rounded-control border border-border bg-surface px-3 py-2 font-mono text-sm text-ink focus:border-danger focus:outline-none"
                    />
                    <div class="flex flex-wrap gap-2">
                      <button
                        type="submit"
                        :disabled="busy || typedKey.trim() !== p.key"
                        class="rounded-control bg-danger px-4 py-2 text-sm font-semibold text-on-accent transition hover:bg-danger/90 disabled:opacity-40"
                      >
                        {{ busy ? 'Deleting…' : 'Delete now' }}
                      </button>
                      <button type="button" :disabled="busy" :class="secondaryButton" @click="deleting = null">Cancel</button>
                    </div>
                  </form>
                </td>
              </tr>
              <tr v-if="history?.key === p.key">
                <td colspan="7" class="px-4 py-3">
                  <p v-if="history.revisions.length === 0" class="text-sm text-ink-muted">
                    {{ history.loading ? 'Loading…' : 'Nothing has been pushed to it yet.' }}
                  </p>
                  <ol v-else class="content-history divide-y divide-border rounded-panel border border-border">
                    <li v-for="r in history.revisions" :key="r.number" class="flex flex-wrap items-baseline gap-x-3 gap-y-1 px-4 py-2">
                      <span class="font-mono text-xs font-semibold">r{{ r.number }}</span>
                      <span class="min-w-0 flex-1 break-words">{{ r.message }}</span>
                      <span class="text-xs text-ink-muted">
                        {{ r.author }}, {{ r.file_count }} file{{ r.file_count === 1 ? '' : 's' }}, {{ formatBytes(r.bytes) }},
                        <RelativeTime :value="r.created_at" :now="now" />
                      </span>
                    </li>
                  </ol>
                  <button
                    v-if="history.more"
                    type="button"
                    :disabled="history.loading"
                    :class="[secondaryButton, 'mt-3']"
                    @click="olderRevisions"
                  >
                    {{ history.loading ? 'Loading…' : 'Older revisions' }}
                  </button>
                </td>
              </tr>
            </template>
          </tbody>
        </table>
      </div>
    </section>

    <section class="space-y-4 rounded-panel border border-border bg-panel p-4 sm:p-6" aria-labelledby="content-new">
      <h2 id="content-new" class="text-xl font-semibold text-ink">New project</h2>
      <p class="text-sm text-ink-muted">
        An empty project: an admin's editor binds a project folder to it and pushes its files as revision 1. A retail
        game's revision 1 is a fresh import of the stock install, so its releases carry what changed since.
      </p>
      <form class="grid gap-4 sm:grid-cols-2" novalidate @submit.prevent="create">
        <div class="space-y-1">
          <label for="content-new-key" class="block text-xs font-medium uppercase tracking-wider text-ink-muted">Key</label>
          <input
            id="content-new-key"
            v-model="draft.key"
            type="text"
            autocomplete="off"
            autocapitalize="characters"
            spellcheck="false"
            maxlength="16"
            aria-describedby="content-new-key-hint"
            :aria-invalid="keyProblem ? 'true' : undefined"
            :class="fieldClass"
          />
          <p id="content-new-key-hint" class="text-xs" :class="keyProblem ? 'text-danger' : 'text-ink-muted'">
            {{ keyProblem || 'Up to 16 of A-Z, 0-9 and _; it never changes.' }}
          </p>
        </div>
        <div class="space-y-1">
          <label for="content-new-title" class="block text-xs font-medium uppercase tracking-wider text-ink-muted">Title</label>
          <input id="content-new-title" v-model="draft.title" type="text" autocomplete="off" :maxlength="TITLE_MAX" :class="fieldClass" />
        </div>
        <div class="space-y-1">
          <label for="content-new-visibility" class="block text-xs font-medium uppercase tracking-wider text-ink-muted">Who reads it</label>
          <select id="content-new-visibility" v-model="draft.visibility" :class="fieldClass">
            <option value="public">Anyone</option>
            <option value="private">Admins only</option>
          </select>
        </div>
        <div class="space-y-1">
          <label for="content-new-base" class="block text-xs font-medium uppercase tracking-wider text-ink-muted">Builds on</label>
          <select id="content-new-base" v-model="draft.base" :class="fieldClass">
            <option :value="null">Nothing (a base game)</option>
            <option v-for="p in projects ?? []" :key="p.key" :value="p.key">{{ p.title }} ({{ p.key }})</option>
          </select>
        </div>
        <div class="space-y-1">
          <label for="content-new-data-set" class="block text-xs font-medium uppercase tracking-wider text-ink-muted">Updates</label>
          <select id="content-new-data-set" v-model="draft.data_set_id" :class="fieldClass">
            <option :value="null">No data set</option>
            <option v-for="d in dataSets" :key="d.id" :value="d.id">{{ d.display_name }}</option>
          </select>
          <p class="text-xs text-ink-muted">The data set its releases update for players.</p>
        </div>
        <label class="flex items-center gap-2 self-center text-sm text-ink">
          <input v-model="draft.retail" type="checkbox" class="h-4 w-4 rounded border-border" />
          A retail game's import
        </label>
        <div class="sm:col-span-2">
          <button type="submit" :disabled="busy || !draftKey || !draft.title.trim()" :class="primaryButton">
            {{ busy ? 'Making…' : 'Make project' }}
          </button>
        </div>
      </form>
    </section>
  </div>
</template>

<script setup lang="ts">
// /admin/content: the content store's projects (ADR 0069; GET /api/content/projects with the
// admin's session, the private ones among them), each with who reads it, its newest revision, the
// project it builds on, the data set its releases update and what the store holds for it; its
// history (GET .../revisions, a page at a time), its title and visibility changed (PATCH), and its
// deletion behind its key typed (DELETE); and a new project (POST). The files themselves move only
// through the OpenNova Editor's Content window.
import AdminPageHeader from '../../components/AdminPageHeader.vue';
import RelativeTime from '../../components/RelativeTime.vue';
import { computed, reactive, ref } from 'vue';
import {
  REVISIONS_PAGE,
  buildContentRelease,
  createContentProject,
  deleteContentProject,
  fetchContentProjects,
  fetchContentRevisions,
  updateContentProject,
} from '../../api/content';
import { fetchDataSets } from '../../api/dataSets';
import { usePolling } from '../../composables/usePolling';
import type { ContentProject, ContentRevision, NewContentProject } from '../../types/content';
import type { DataSet } from '../../types/dataSets';
import { formatBytes } from '../../utils/format';

const POLL_MS = 60_000;
// The routes' title length (ContentRoutes::kTitleMax).
const TITLE_MAX = 64;
const KEY_RULE = /^[A-Z0-9_]{1,16}$/;

const fieldClass =
  'w-full rounded-control border border-border bg-surface px-3 py-2 text-sm text-ink focus:border-accent focus:outline-none aria-[invalid=true]:border-danger';
const primaryButton =
  'rounded-control bg-accent px-4 py-2 text-sm font-medium text-on-accent transition hover:bg-accent/90 disabled:cursor-not-allowed disabled:opacity-50';
const secondaryButton =
  'rounded-control border border-border bg-raised px-4 py-2 text-sm font-medium text-ink transition hover:bg-hover disabled:cursor-not-allowed disabled:opacity-40';

const projects = ref<ContentProject[] | null>(null);
const dataSets = ref<DataSet[]>([]);
const busy = ref(false);
const loadedAt = ref<number | null>(null);
const now = ref(Date.now());
const message = ref('');
const error = ref('');
const pollTrouble = ref(false);
const renaming = ref<string | null>(null);
const newTitle = ref('');
const deleting = ref<string | null>(null);
const typedKey = ref('');
// The project whose release form is open, and its version and revision.
const releasing = ref<string | null>(null);
const releaseVersion = ref('');
const releaseRevision = ref(0);
const releaseTried = ref(false);
const VERSION_RULE = /^[0-9]+(\.[0-9]+){0,3}$/;
const versionProblem = computed(() =>
  releaseTried.value && !VERSION_RULE.test(releaseVersion.value.trim()) ? 'A version is one to four numbers between dots.' : '',
);
// The history shown: its project, the revisions read so far, and whether older ones remain.
const history = ref<{ key: string; revisions: ContentRevision[]; more: boolean; loading: boolean } | null>(null);
let errorFromLoad = false;

const blankDraft = (): NewContentProject => ({
  key: '',
  title: '',
  visibility: 'private',
  retail: false,
  data_set_id: null,
  base: null,
});
const draft = reactive<NewContentProject>(blankDraft());
const keyTried = ref(false);
// The key as it is sent: a lower-case letter typed is its capital.
const draftKey = computed(() => draft.key.trim().toUpperCase());
const keyProblem = computed(() =>
  keyTried.value && !KEY_RULE.test(draftKey.value) ? 'A key is 1 to 16 of A-Z, 0-9 and _.' : '',
);

function flash(text: string) {
  message.value = text;
  error.value = '';
}

function fail(text: string) {
  error.value = text;
  message.value = '';
  errorFromLoad = false;
}

function dataSetName(id: number | null): string {
  if (id === null) return 'Nothing';
  return dataSets.value.find((d) => d.id === id)?.display_name ?? `Data set ${id}`;
}

const poll = usePolling({
  async read(signal, background) {
    const [result, sets] = await Promise.all([fetchContentProjects(signal), fetchDataSets(signal)]);
    if (signal.aborted) return false;
    now.value = Date.now();
    if (sets.ok) dataSets.value = sets.value.data_sets;
    if (result.ok) {
      projects.value = result.value;
      loadedAt.value = now.value;
      pollTrouble.value = false;
      if (errorFromLoad) {
        error.value = '';
        errorFromLoad = false;
      }
      return true;
    }
    if (background && projects.value !== null) {
      pollTrouble.value = true;
    } else {
      fail(result.error.message);
      errorFromLoad = true;
    }
    return false;
  },
  next: () => POLL_MS,
});

async function readHistory(key: string, before?: number) {
  const shown = history.value;
  if (!shown || shown.key !== key) return;
  shown.loading = true;
  const result = await fetchContentRevisions(key, before);
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

function toggleHistory(p: ContentProject) {
  if (history.value?.key === p.key) {
    history.value = null;
    return;
  }
  history.value = { key: p.key, revisions: [], more: false, loading: true };
  void readHistory(p.key);
}

function olderRevisions() {
  const shown = history.value;
  const last = shown?.revisions[shown.revisions.length - 1];
  if (shown && last) void readHistory(shown.key, last.number);
}

function beginRelease(p: ContentProject) {
  renaming.value = null;
  deleting.value = null;
  releasing.value = p.key;
  releaseVersion.value = '';
  releaseRevision.value = p.head;
  releaseTried.value = false;
}

async function confirmRelease(p: ContentProject) {
  if (busy.value) return;
  releaseTried.value = true;
  const version = releaseVersion.value.trim();
  if (!VERSION_RULE.test(version)) return;
  busy.value = true;
  const result = await buildContentRelease(p.key, { version, revision: releaseRevision.value || p.head });
  busy.value = false;
  if (!result.ok) {
    fail(result.error.message);
    return;
  }
  releasing.value = null;
  flash(`${result.value.name} is building: Game data's Releases shows it, a draft to publish once it is checked.`);
}

function beginRename(p: ContentProject) {
  releasing.value = null;
  deleting.value = null;
  renaming.value = p.key;
  newTitle.value = p.title;
}

function beginDelete(p: ContentProject) {
  renaming.value = null;
  releasing.value = null;
  deleting.value = p.key;
  typedKey.value = '';
}

async function saveRename(p: ContentProject) {
  const title = newTitle.value.trim();
  if (busy.value || !title) return;
  busy.value = true;
  const result = await updateContentProject(p.key, { title });
  busy.value = false;
  if (!result.ok) {
    fail(result.error.message);
    return;
  }
  renaming.value = null;
  flash(`${p.key} is now titled ${result.value.title}.`);
  void poll.readAgain();
}

async function toggleVisibility(p: ContentProject) {
  if (busy.value) return;
  busy.value = true;
  const visibility = p.visibility === 'public' ? 'private' : 'public';
  const result = await updateContentProject(p.key, { visibility });
  busy.value = false;
  if (!result.ok) {
    fail(result.error.message);
    return;
  }
  flash(visibility === 'public' ? `Anyone may now get ${p.key}.` : `Only admins may now get ${p.key}.`);
  void poll.readAgain();
}

async function confirmDelete(p: ContentProject) {
  if (busy.value || typedKey.value.trim() !== p.key) return;
  busy.value = true;
  const result = await deleteContentProject(p.key);
  busy.value = false;
  if (!result.ok) {
    fail(result.error.message);
    return;
  }
  deleting.value = null;
  if (history.value?.key === p.key) history.value = null;
  flash(`${p.key} is deleted.`);
  void poll.readAgain();
}

async function create() {
  if (busy.value) return;
  keyTried.value = true;
  if (!KEY_RULE.test(draftKey.value) || !draft.title.trim()) return;
  busy.value = true;
  const result = await createContentProject({ ...draft, key: draftKey.value, title: draft.title.trim() });
  busy.value = false;
  if (!result.ok) {
    fail(result.error.message);
    return;
  }
  flash(`${result.value.key} is made: bind a project folder to it in the editor and push.`);
  Object.assign(draft, blankDraft());
  keyTried.value = false;
  void poll.readAgain();
}
</script>
