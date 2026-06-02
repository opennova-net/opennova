<template>
  <AdminGate>
  <div class="mx-auto max-w-6xl px-6 py-10 space-y-10">
    <header class="flex flex-col gap-4 sm:flex-row sm:items-center sm:justify-between">
      <div>
        <h1 class="text-3xl font-semibold text-white">Expansion Releases</h1>
        <p class="text-sm text-slate-300">Manage expansion versions, trigger releases, and review recent activity.</p>
      </div>
      <button
        type="button"
        class="inline-flex items-center justify-center rounded-md border border-white/10 bg-white/5 px-4 py-2 text-sm font-medium text-slate-50 transition hover:bg-white/10 disabled:cursor-not-allowed disabled:opacity-40"
        :disabled="refreshing || loading"
        @click="refreshDashboard"
      >
        <span v-if="refreshing">Refreshing…</span>
        <span v-else>Refresh</span>
      </button>
    </header>

    <div v-if="message" class="rounded-md border border-emerald-400/40 bg-emerald-500/10 p-4 text-sm text-emerald-200">
      {{ message }}
    </div>
    <div v-if="error" class="rounded-md border border-rose-400/40 bg-rose-500/10 p-4 text-sm text-rose-200">
      {{ error }}
    </div>

    <div v-if="loading" class="flex h-48 items-center justify-center text-slate-300">
      Loading admin data…
    </div>

    <template v-else>
      <section class="space-y-4">
        <h2 class="text-xl font-semibold text-white">Available Expansions</h2>
        <div class="overflow-hidden rounded-xl border border-white/10 bg-white/5">
          <table class="min-w-full divide-y divide-white/10">
            <thead>
              <tr class="bg-white/5 text-left text-xs font-semibold uppercase tracking-wider text-slate-300">
                <th class="px-4 py-3">Slug</th>
                <th class="px-4 py-3">Display</th>
                <th class="px-4 py-3">Current Version</th>
                <th class="px-4 py-3">Install Target</th>
                <th class="px-4 py-3">Actions</th>
              </tr>
            </thead>
            <tbody class="divide-y divide-white/5 text-sm text-slate-200">
              <tr v-if="expansions.length === 0">
                <td colspan="5" class="px-4 py-6 text-center text-slate-400">
                  No expansions configured yet.
                </td>
              </tr>
              <tr v-for="expansion in expansions" :key="expansion.slug" class="hover:bg-white/5">
                <td class="px-4 py-3 font-mono text-xs">{{ expansion.slug }}</td>
                <td class="px-4 py-3">{{ expansion.displayName }}</td>
                <td class="px-4 py-3">{{ expansion.version }}</td>
                <td class="px-4 py-3 font-mono text-xs">{{ expansion.install.subdir }}</td>
                <td class="px-4 py-3">
                  <form
                    v-if="formState[expansion.slug]"
                    class="grid gap-3 sm:grid-cols-[minmax(0,1fr)_minmax(0,1fr)] sm:items-start"
                    @submit.prevent="submitRelease(expansion.slug)"
                  >
                    <div class="flex flex-col gap-1 text-xs sm:text-sm">
                      <label class="font-medium text-slate-300">Version</label>
                      <input
                        v-model="formState[expansion.slug].version"
                        type="text"
                        required
                        class="w-full min-w-[8rem] rounded-md border border-white/10 bg-slate-900/60 px-3 py-2 text-sm text-white shadow-inner focus:border-brand-400 focus:outline-none"
                      >
                    </div>
                    <div class="flex flex-col gap-1 text-xs sm:text-sm sm:col-span-2">
                      <label class="font-medium text-slate-300">Notes</label>
                      <textarea
                        v-model="formState[expansion.slug].notes"
                        placeholder="Optional release notes"
                        rows="4"
                        class="w-full rounded-md border border-white/10 bg-slate-900/60 px-3 py-2 text-sm text-white shadow-inner focus:border-brand-400 focus:outline-none"
                      />
                    </div>
                    <div class="mt-2 sm:col-span-2 sm:flex sm:justify-end">
                      <button
                        type="submit"
                        class="inline-flex items-center justify-center rounded-md bg-brand-500 px-4 py-2 text-sm font-medium text-white transition hover:bg-brand-400 disabled:cursor-not-allowed disabled:opacity-60"
                        :disabled="isSubmitting(expansion.slug) || loading || refreshing"
                      >
                        <span v-if="isSubmitting(expansion.slug)">Submitting…</span>
                        <span v-else>Release</span>
                      </button>
                    </div>
                  </form>
                  <div v-else class="text-xs text-slate-400">Preparing form…</div>
                </td>
              </tr>
            </tbody>
          </table>
        </div>
      </section>

      <section class="space-y-4">
        <h2 class="text-xl font-semibold text-white">Recent Releases</h2>
        <div class="overflow-hidden rounded-xl border border-white/10 bg-white/5">
          <table class="min-w-full divide-y divide-white/10">
            <thead>
              <tr class="bg-white/5 text-left text-xs font-semibold uppercase tracking-wider text-slate-300">
                <th class="px-4 py-3">Slug</th>
                <th class="px-4 py-3">Version</th>
                <th class="px-4 py-3">Tag</th>
                <th class="px-4 py-3">Status</th>
                <th class="px-4 py-3">Commit</th>
                <th class="px-4 py-3">Published</th>
                <th class="px-4 py-3">Notes</th>
                <th class="px-4 py-3">Error</th>
              </tr>
            </thead>
            <tbody class="divide-y divide-white/5 text-xs text-slate-200">
              <tr v-if="releases.length === 0">
                <td colspan="8" class="px-4 py-6 text-center text-slate-400">
                  No releases recorded yet.
                </td>
              </tr>
              <tr v-for="release in releases" :key="release.id" class="hover:bg-white/5">
                <td class="px-4 py-3 font-mono">{{ release.slug }}</td>
                <td class="px-4 py-3 font-mono">{{ release.version }}</td>
                <td class="px-4 py-3 font-mono text-xs">{{ release.repoRef }}</td>
                <td class="px-4 py-3">
                  <span :class="statusClass(release.status)" class="inline-flex items-center rounded-full px-2.5 py-1 text-[11px] font-semibold uppercase tracking-wider">
                    {{ release.status }}
                  </span>
                  <div v-if="release.workflowUrl" class="mt-1">
                    <a :href="release.workflowUrl" target="_blank" rel="noreferrer" class="text-[11px] text-brand-200 hover:text-brand-100">
                      view workflow
                    </a>
                  </div>
                </td>
                <td class="px-4 py-3 font-mono text-xs">
                  <span v-if="release.targetCommit">{{ release.targetCommit.slice(0, 8) }}</span>
                  <span v-else>&mdash;</span>
                </td>
                <td class="px-4 py-3">{{ formatDate(release.publishedAt) }}</td>
                <td class="px-4 py-3 text-xs">
                  <div v-if="!hasNotes(release)">
                    &mdash;
                  </div>
                  <div v-else class="space-y-2">
                    <button
                      type="button"
                      class="inline-flex items-center rounded-md border border-white/10 bg-white/5 px-2 py-1 text-[11px] font-medium uppercase tracking-wide text-brand-100 transition hover:bg-white/10"
                      @click="toggleNotes(release.id)"
                    >
                      <span v-if="isNotesOpen(release.id)">Hide notes</span>
                      <span v-else>View notes</span>
                    </button>
                    <div
                      v-if="isNotesOpen(release.id)"
                      class="whitespace-pre-wrap rounded-md border border-white/10 bg-slate-900/70 p-3 text-slate-200"
                    >
                      {{ release.notes }}
                    </div>
                  </div>
                </td>
                <td class="px-4 py-3 whitespace-pre-wrap text-rose-200">{{ release.errorMessage || '—' }}</td>
              </tr>
            </tbody>
          </table>
        </div>
      </section>
    </template>
  </div>
  </AdminGate>
</template>

<script setup lang="ts">
import { isAxiosError } from 'axios';
import { onMounted, reactive, ref } from 'vue';
import AdminGate from '../components/AdminGate.vue';
import { createExpansionRelease, fetchAdminExpansions, fetchAdminReleases } from '../api/admin';
import type { AdminExpansion, AdminRelease } from '../types/admin';

const loading = ref(true);
const refreshing = ref(false);
const expansions = ref<AdminExpansion[]>([]);
const releases = ref<AdminRelease[]>([]);
const message = ref<string | null>(null);
const error = ref<string | null>(null);
const submittingSlug = ref<string | null>(null);

const formState = reactive<Record<string, { version: string; notes: string }>>({});
const openNotes = reactive<Record<number, boolean>>({});

function ensureFormState(expansion: AdminExpansion) {
  if (!formState[expansion.slug]) {
    formState[expansion.slug] = {
      version: expansion.version,
      notes: '',
    };
  }
}

function extractError(err: unknown): string {
  if (isAxiosError(err)) {
    const data = err.response?.data as Record<string, any> | undefined;
    if (data) {
      if (typeof data.error === 'string') {
        return data.error;
      }
      if (typeof data.message === 'string') {
        return data.message;
      }
    }
    return err.response?.statusText || 'Request failed';
  }
  if (err instanceof Error) {
    return err.message;
  }
  return 'Unexpected error';
}

async function loadDashboard(showSpinner = true) {
  if (showSpinner) {
    loading.value = true;
  } else {
    refreshing.value = true;
  }

  try {
    const [expansionData, releaseData] = await Promise.all([
      fetchAdminExpansions(),
      fetchAdminReleases(),
    ]);

    expansionData.forEach(ensureFormState);
    expansions.value = expansionData;
    releases.value = releaseData;
    resetNotesState(releaseData);
    error.value = null;
  } catch (err) {
    error.value = extractError(err);
  } finally {
    if (showSpinner) {
      loading.value = false;
    } else {
      refreshing.value = false;
    }
  }
}

function hasNotes(release: AdminRelease): boolean {
  return typeof release.notes === 'string' && release.notes.trim().length > 0;
}

function toggleNotes(id: number) {
  openNotes[id] = !openNotes[id];
}

function isNotesOpen(id: number): boolean {
  return !!openNotes[id];
}

function resetNotesState(releaseData: AdminRelease[]) {
  for (const key of Object.keys(openNotes)) {
    delete openNotes[Number(key)];
  }
  releaseData.forEach((release) => {
    if (hasNotes(release)) {
      openNotes[release.id] = false;
    }
  });
}

onMounted(() => {
  loadDashboard(true);
});

function formatDate(value?: string | null) {
  if (!value) {
    return '—';
  }
  const date = new Date(value);
  if (Number.isNaN(date.getTime())) {
    return value;
  }
  return date.toLocaleString();
}

function statusClass(status: string) {
  switch (status) {
    case 'published':
      return 'bg-emerald-500/15 text-emerald-200 border border-emerald-400/30';
    case 'tagged':
      return 'bg-sky-500/15 text-sky-200 border border-sky-400/30';
    case 'failed':
      return 'bg-rose-500/15 text-rose-200 border border-rose-400/30';
    default:
      return 'bg-amber-500/15 text-amber-200 border border-amber-400/30';
  }
}

function isSubmitting(slug: string) {
  return submittingSlug.value === slug;
}

async function submitRelease(slug: string) {
  if (!formState[slug]) {
    return;
  }

  const payload = {
    version: formState[slug].version.trim(),
    notes: formState[slug].notes.trim() || undefined,
  };

  message.value = null;
  error.value = null;
  submittingSlug.value = slug;

  try {
    const response = await createExpansionRelease(slug, payload);
    if (!response.ok) {
      throw new Error(response.message || 'Release failed');
    }
    message.value = response.message || 'Release triggered successfully.';
    await loadDashboard(false);
    if (!formState[slug]) {
      formState[slug] = { version: payload.version, notes: '' };
    }
    formState[slug].version = response.release.version;
    formState[slug].notes = '';
  } catch (err) {
    error.value = extractError(err);
  } finally {
    submittingSlug.value = null;
  }
}

function refreshDashboard() {
  loadDashboard(false);
}
</script>

<style scoped>
table {
  border-spacing: 0;
}
</style>
