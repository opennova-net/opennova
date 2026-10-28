<template>
  <div class="space-y-6">
    <PageHeader title="New project" :crumbs="CRUMBS" />

    <LiveNotices :error="notice || loadError" />

    <p v-if="mine === null && !loadError" class="text-sm text-ink-muted">Loading…</p>
    <button v-else-if="mine === null" type="button" class="btn-secondary" @click="load">Try again</button>

    <Notice v-else-if="atLimit" tone="warn" class="new-project-blocked">You have {{ mine.limits.projects }} projects, the most an account may have.</Notice>

    <form v-else id="new-project-form" class="card max-w-3xl space-y-5 p-4 sm:p-6" novalidate @submit.prevent="submit">
      <div class="grid gap-5 sm:grid-cols-2">
        <div>
          <label for="new-project-name" class="field-label">Name</label>
          <input
            id="new-project-name"
            v-model="form.name"
            type="text"
            autocomplete="off"
            :maxlength="NAME_MAX"
            :disabled="busy"
            :aria-invalid="errors.display_name ? 'true' : undefined"
            :aria-describedby="errors.display_name ? 'new-project-name-error' : undefined"
            class="field"
            @input="onName"
          />
          <p v-if="errors.display_name" id="new-project-name-error" class="mt-1 text-xs text-danger">{{ errors.display_name }}</p>
        </div>
        <div>
          <label for="new-project-key" class="field-label">Key</label>
          <input
            id="new-project-key"
            v-model="form.key"
            type="text"
            autocomplete="off"
            autocapitalize="characters"
            spellcheck="false"
            :maxlength="OWNED_KEY_MAX"
            :disabled="busy"
            :aria-invalid="errors.key ? 'true' : undefined"
            aria-describedby="new-project-key-hint"
            class="field font-mono"
            @input="keyTyped = true"
          />
          <p id="new-project-key-hint" class="mt-1 text-xs" :class="errors.key ? 'text-danger' : 'text-ink-muted'">
            {{ errors.key || 'A-Z, 0-9 and _; it cannot change later.' }}
          </p>
        </div>
      </div>

      <fieldset class="min-w-0">
        <legend class="field-label">What it is</legend>
        <div class="flex flex-col gap-2 sm:flex-row sm:gap-6">
          <label class="flex items-center gap-2 text-sm text-ink">
            <input v-model="form.kind" type="radio" name="new-project-kind" value="expansion" :disabled="busy" class="accent-accent" />
            A mod of a game
          </label>
          <label class="flex items-center gap-2 text-sm text-ink">
            <input v-model="form.kind" type="radio" name="new-project-kind" value="game" :disabled="busy" class="accent-accent" />
            A game of its own
          </label>
        </div>
      </fieldset>

      <div v-if="form.kind === 'expansion'" class="grid gap-5 sm:grid-cols-2">
        <div>
          <label for="new-project-base" class="field-label">A mod of</label>
          <select id="new-project-base" v-model="form.base" :disabled="busy" :aria-invalid="errors.base ? 'true' : undefined" class="field">
            <option v-if="bases.length === 0" value="" disabled>Nothing to pick</option>
            <option v-for="g in bases" :key="g.key" :value="g.key">{{ g.title }}</option>
          </select>
          <p v-if="errors.base" class="mt-1 text-xs text-danger">{{ errors.base }}</p>
        </div>
        <div>
          <label for="new-project-short-name" class="field-label">Short name</label>
          <input
            id="new-project-short-name"
            v-model="form.shortName"
            type="text"
            autocomplete="off"
            autocapitalize="none"
            spellcheck="false"
            maxlength="8"
            :disabled="busy"
            :aria-invalid="errors.expansion_code ? 'true' : undefined"
            aria-describedby="new-project-short-name-hint"
            class="field font-mono"
            @input="shortNameTyped = true"
          />
          <p id="new-project-short-name-hint" class="mt-1 text-xs" :class="errors.expansion_code ? 'text-danger' : 'text-ink-muted'">
            {{ errors.expansion_code || 'Lowercase letters and digits.' }}
          </p>
        </div>
      </div>

      <div v-else class="sm:max-w-sm">
        <label for="new-project-game" class="field-label">A game for</label>
        <select id="new-project-game" v-model="form.game" :disabled="busy" :aria-invalid="errors.game ? 'true' : undefined" class="field">
          <option v-if="titles.length === 0" value="" disabled>Nothing to pick</option>
          <option v-for="t in titles" :key="t" :value="t">{{ titleName(t) }}</option>
        </select>
        <p v-if="errors.game" class="mt-1 text-xs text-danger">{{ errors.game }}</p>
      </div>

      <fieldset class="min-w-0">
        <legend class="field-label">Who gets it</legend>
        <div class="flex flex-col gap-2">
          <label class="flex items-center gap-2 text-sm text-ink">
            <input v-model="form.visibility" type="radio" name="new-project-visibility" value="public" :disabled="busy" class="accent-accent" />
            {{ visibilityLine('public') }}
          </label>
          <label class="flex items-center gap-2 text-sm text-ink">
            <input v-model="form.visibility" type="radio" name="new-project-visibility" value="private" :disabled="busy" class="accent-accent" />
            {{ visibilityLine('private') }}
          </label>
        </div>
      </fieldset>

      <div class="flex flex-wrap items-center gap-2">
        <button id="new-project-submit" type="submit" :disabled="busy" class="btn-primary">{{ busy ? 'Creating…' : 'Create project' }}</button>
        <RouterLink to="/me/projects" class="btn-ghost">Cancel</RouterLink>
      </div>
    </form>
  </div>
</template>

<script setup lang="ts">
// /me/projects/new: a new project of the account's own (POST /api/products),
// offered while it has fewer than the most it may (GET /api/me/projects). Its
// name suggests its key and, for a mod, its short name, each until typed over;
// the form checks the routes' rules first, and a refusal of one field shows
// under that field. A project made goes to its page.
import { computed, onMounted, reactive, ref } from 'vue';
import { RouterLink, useRoute, useRouter } from 'vue-router';
import LiveNotices from '../../components/ui/LiveNotices.vue';
import Notice from '../../components/ui/Notice.vue';
import PageHeader from '../../components/ui/PageHeader.vue';
import { createProject, fetchMyProjects, type ProjectError } from '../../api/products';
import { useSession } from '../../composables/useSession';
import { useUnsavedChanges } from '../../composables/useUnsavedChanges';
import type { MyProjects, NewProject, ProjectVisibility } from '../../types/products';
import type { Crumb } from '../../utils/adminNav';
import { titleName } from '../../utils/games';
import {
  KEY_HINT,
  NAME_MAX,
  OWNED_KEY_MAX,
  SHORT_NAME_HINT,
  SHORT_NAME_RULE,
  isOwnedKey,
  myProjectLink,
  nameProblem,
  suggestKey,
  suggestShortName,
  visibilityLine,
} from '../../utils/projectsView';

const CRUMBS: Crumb[] = [{ label: 'Your projects', to: '/me/projects' }];

// The fields a refusal may name, as the routes name them.
type Field = 'display_name' | 'key' | 'base' | 'expansion_code' | 'game';
const FIELDS: readonly string[] = ['display_name', 'key', 'base', 'expansion_code', 'game'];

const route = useRoute();
const router = useRouter();
const { refresh: refreshSession } = useSession();

const mine = ref<MyProjects | null>(null);
const loadError = ref('');
const notice = ref('');
const busy = ref(false);
const done = ref(false);

const form = reactive({
  name: '',
  key: '',
  kind: 'expansion' as 'game' | 'expansion',
  base: '',
  shortName: '',
  game: '',
  visibility: 'public' as ProjectVisibility,
});
// The key and short name follow the name until typed over.
const keyTyped = ref(false);
const shortNameTyped = ref(false);
const errors = ref<Partial<Record<Field, string>>>({});

const bases = computed(() => mine.value?.games ?? []);
const titles = computed(() => mine.value?.titles ?? []);
const atLimit = computed(() => !!mine.value && mine.value.usage.projects >= mine.value.limits.projects);

useUnsavedChanges(() => !done.value && (form.name.trim() !== '' || form.key.trim() !== '' || form.shortName.trim() !== ''));

function onName() {
  if (!keyTyped.value) form.key = suggestKey(form.name);
  if (!shortNameTyped.value) form.shortName = suggestShortName(form.name);
}

async function onSessionEnded() {
  await refreshSession();
  await router.replace({ path: '/login', query: { redirect: route.fullPath } });
}

async function load() {
  loadError.value = '';
  const result = await fetchMyProjects();
  if (!result.ok) {
    if (result.error.code === 'unauthorized') return onSessionEnded();
    loadError.value = result.error.message;
    return;
  }
  mine.value = result.value;
  // A pick the new reply no longer offers falls back to the first offered.
  if (!bases.value.some((g) => g.key === form.base)) form.base = bases.value[0]?.key ?? '';
  if (!titles.value.includes(form.game)) form.game = titles.value[0] ?? '';
}

// The form against the routes' rules; the body to send when it passes.
function check(): NewProject | null {
  const found: Partial<Record<Field, string>> = {};
  const displayName = form.name.trim();
  const named = nameProblem(form.name);
  if (named) found.display_name = named;
  // A lower-case letter typed is its capital.
  const key = form.key.trim().toUpperCase();
  if (!isOwnedKey(key)) found.key = KEY_HINT;
  const body: NewProject = { key, display_name: displayName, kind: form.kind, visibility: form.visibility };
  if (form.kind === 'expansion') {
    const code = form.shortName.trim().toLowerCase();
    if (bases.value.length === 0) found.base = 'NovaWorld lists no game to make a mod of right now.';
    else if (!bases.value.some((g) => g.key === form.base)) found.base = 'Pick the game the mod is for.';
    if (!SHORT_NAME_RULE.test(code)) found.expansion_code = SHORT_NAME_HINT;
    body.base = form.base;
    body.expansion_code = code;
  } else {
    if (titles.value.length === 0) found.game = 'NovaWorld lists no game to make one for right now.';
    else if (!titles.value.includes(form.game)) found.game = 'Pick the game it is for.';
    body.game = form.game;
  }
  errors.value = found;
  return Object.keys(found).length === 0 ? body : null;
}

// A refusal: under its field when it names one of the form's, else above the
// form; one that means the page's picture is out of date reads it again.
function refused(error: ProjectError) {
  const field =
    error.code === 'duplicate_key' ? 'key' : error.code === 'duplicate_expansion' ? 'expansion_code' : error.code === 'no_such_base' ? 'base' : error.field;
  if (FIELDS.includes(field)) {
    errors.value = { [field]: error.message };
    if (error.code === 'no_such_base') void load();
    return;
  }
  notice.value = error.message;
  if (error.code === 'too_many_projects') void load();
}

async function submit() {
  notice.value = '';
  const body = check();
  if (!body || busy.value) return;
  busy.value = true;
  const result = await createProject(body);
  busy.value = false;
  if (!result.ok) {
    if (result.error.code === 'unauthorized') {
      done.value = true;
      void onSessionEnded();
      return;
    }
    refused(result.error);
    return;
  }
  done.value = true;
  await router.push(myProjectLink(result.value.key));
}

onMounted(load);
</script>
