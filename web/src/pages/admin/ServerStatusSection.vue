<template>
  <div class="space-y-10">
    <header class="flex flex-col gap-4 sm:flex-row sm:items-center sm:justify-between">
      <div>
        <h1 class="text-3xl font-semibold text-ink">Server Status</h1>
        <p class="text-sm text-ink-muted">Toggle maintenance mode and set the message players see.</p>
      </div>
      <button
        type="button"
        class="rounded-control border border-border bg-raised px-4 py-2 text-sm font-medium text-ink transition hover:bg-hover disabled:cursor-not-allowed disabled:opacity-40"
        :disabled="loading || saving"
        @click="load"
      >
        Refresh
      </button>
    </header>

    <div v-if="message" class="rounded-panel border border-online/40 bg-online/10 p-4 text-sm text-online">
      {{ message }}
    </div>
    <div v-if="error" class="rounded-panel border border-danger/40 bg-danger/10 p-4 text-sm text-danger">
      {{ error }}
    </div>

    <div v-if="loading" class="flex h-32 items-center justify-center text-ink-muted">
      Loading server status…
    </div>

    <section v-else class="space-y-4">
      <div
        class="flex items-center gap-3 rounded-panel border p-4 text-sm"
        :class="form.maintenance_enabled
          ? 'border-warn/40 bg-warn/10 text-warn'
          : 'border-online/40 bg-online/10 text-online'"
      >
        <span class="inline-flex h-2.5 w-2.5 rounded-full" :class="form.maintenance_enabled ? 'bg-warn' : 'bg-online'"></span>
        <span class="font-medium">
          {{ form.maintenance_enabled ? 'Maintenance mode is ON' : 'Server is live' }}
        </span>
      </div>

      <form
        class="space-y-5 rounded-panel border border-border bg-panel p-6"
        @submit.prevent="save"
      >
        <label class="flex items-center gap-3">
          <input
            v-model="form.maintenance_enabled"
            type="checkbox"
            class="h-4 w-4 rounded border-border bg-surface text-accent focus:ring-accent"
          />
          <span class="text-sm font-medium text-ink">Enable maintenance mode</span>
        </label>

        <label class="block space-y-1">
          <span class="text-xs font-medium uppercase tracking-wider text-ink-muted">Status message</span>
          <textarea
            v-model="form.message"
            rows="3"
            placeholder="Shown to players when maintenance is on"
            class="w-full rounded-control border border-border bg-surface px-3 py-2 text-sm text-ink focus:border-accent focus:outline-none"
          />
        </label>

        <div class="flex justify-end">
          <button
            type="submit"
            class="rounded-control bg-accent px-4 py-2 text-sm font-medium text-on-accent transition hover:bg-accent/90 disabled:cursor-not-allowed disabled:opacity-60"
            :disabled="saving || loading"
          >
            <span v-if="saving">Saving…</span>
            <span v-else>Save</span>
          </button>
        </div>
      </form>
    </section>
  </div>
</template>

<script setup lang="ts">
import { isAxiosError } from 'axios';
import { onMounted, reactive, ref } from 'vue';
import { fetchServerStatus, updateServerStatus } from '../../api/admin';

const loading = ref(true);
const saving = ref(false);
const message = ref<string | null>(null);
const error = ref<string | null>(null);

const form = reactive({ maintenance_enabled: false, message: '' });

function extractError(err: unknown): string {
  if (isAxiosError(err)) {
    const data = err.response?.data as Record<string, any> | undefined;
    if (typeof data?.message === 'string') return data.message;
    if (typeof data?.error === 'string') return data.error;
    return err.response?.statusText || 'Request failed';
  }
  if (err instanceof Error) return err.message;
  return 'Unexpected error';
}

async function load() {
  loading.value = true;
  error.value = null;
  try {
    const status = await fetchServerStatus();
    form.maintenance_enabled = status.maintenance_enabled;
    form.message = status.message;
  } catch (err) {
    error.value = extractError(err);
  } finally {
    loading.value = false;
  }
}

async function save() {
  saving.value = true;
  message.value = null;
  error.value = null;
  try {
    const status = await updateServerStatus({
      maintenance_enabled: form.maintenance_enabled,
      message: form.message,
    });
    form.maintenance_enabled = status.maintenance_enabled;
    form.message = status.message;
    message.value = 'Server status updated.';
  } catch (err) {
    error.value = extractError(err);
  } finally {
    saving.value = false;
  }
}

onMounted(load);
</script>
