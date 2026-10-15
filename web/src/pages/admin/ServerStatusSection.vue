<template>
  <div class="space-y-10">
    <header class="flex flex-col gap-4 sm:flex-row sm:items-center sm:justify-between">
      <div>
        <h1 class="text-3xl font-semibold text-ink">NovaWorld service</h1>
        <p class="text-sm text-ink-muted">Maintenance mode and the message game clients see when signing in.</p>
      </div>
      <button type="button" :class="secondaryButton" :disabled="loading || saving" @click="load">Refresh</button>
    </header>

    <div role="status" class="sr-only">{{ message }}</div>
    <div role="alert" class="sr-only">{{ error }}</div>
    <div v-if="message" aria-hidden="true" class="rounded-panel border border-online/40 bg-online/10 p-4 text-sm text-online">{{ message }}</div>
    <div v-if="error" aria-hidden="true" class="rounded-panel border border-danger/40 bg-danger/10 p-4 text-sm text-danger">{{ error }}</div>

    <div v-if="loading && !loaded" class="flex h-32 items-center justify-center text-ink-muted">Loading the service's status…</div>

    <section v-else-if="loaded" class="space-y-4" aria-labelledby="service-form">
      <p
        class="flex items-center gap-3 rounded-panel border p-4 text-sm"
        :class="form.maintenance_enabled ? 'border-warn/40 bg-warn/10 text-warn' : 'border-online/40 bg-online/10 text-online'"
      >
        <span aria-hidden="true" class="inline-flex h-2.5 w-2.5 rounded-full" :class="form.maintenance_enabled ? 'bg-warn' : 'bg-online'"></span>
        <span class="font-medium">
          {{ saved.maintenance_enabled ? 'Maintenance mode is on: game clients cannot sign in.' : 'NovaWorld is open: game clients can sign in.' }}
        </span>
      </p>

      <form class="space-y-5 rounded-panel border border-border bg-panel p-6" @submit.prevent="save">
        <h2 id="service-form" class="sr-only">Maintenance mode</h2>
        <label class="flex items-center gap-3">
          <input
            v-model="form.maintenance_enabled"
            type="checkbox"
            aria-describedby="service-maintenance-hint"
            class="h-4 w-4 rounded border-border bg-surface text-accent focus:ring-accent"
          />
          <span class="text-sm font-medium text-ink">Maintenance mode</span>
        </label>
        <p id="service-maintenance-hint" class="text-xs text-ink-muted">
          While it is on, a game client's NovaWorld login is refused with the message below.
        </p>

        <label class="block space-y-1">
          <span class="text-xs font-medium uppercase tracking-wider text-ink-muted">Message game clients see</span>
          <textarea
            v-model="form.message"
            rows="3"
            placeholder="Shown when a game client signs in during maintenance"
            class="w-full rounded-control border border-border bg-surface px-3 py-2 text-sm text-ink focus:border-accent focus:outline-none"
          />
        </label>

        <div class="flex justify-end">
          <button type="submit" :class="primaryButton" :disabled="saving || loading">{{ saving ? 'Saving…' : 'Save' }}</button>
        </div>
      </form>
    </section>
  </div>
</template>

<script setup lang="ts">
// /admin/service: the NovaWorld service's maintenance mode and the message
// game clients see when signing in (GET/PUT /api/admin/server-status).
import { onMounted, reactive, ref } from 'vue';
import { fetchServerStatus, updateServerStatus } from '../../api/admin';
import type { ServerStatus } from '../../types/admin';

const primaryButton =
  'rounded-control bg-accent px-4 py-2 text-sm font-medium text-on-accent transition hover:bg-accent/90 disabled:cursor-not-allowed disabled:opacity-60';
const secondaryButton =
  'rounded-control border border-border bg-raised px-4 py-2 text-sm font-medium text-ink transition hover:bg-hover disabled:cursor-not-allowed disabled:opacity-40';

const loading = ref(true);
const loaded = ref(false);
const saving = ref(false);
const message = ref('');
const error = ref('');

const form = reactive<ServerStatus>({ maintenance_enabled: false, message: '' });
// The status as the server last answered it (the banner says that, not the
// unsaved form).
const saved = reactive<ServerStatus>({ maintenance_enabled: false, message: '' });

function adopt(status: ServerStatus) {
  Object.assign(form, status);
  Object.assign(saved, status);
}

async function load() {
  loading.value = true;
  error.value = '';
  const result = await fetchServerStatus();
  loading.value = false;
  if (!result.ok) {
    error.value = result.error.message;
    return;
  }
  adopt(result.value);
  loaded.value = true;
}

async function save() {
  saving.value = true;
  message.value = '';
  error.value = '';
  const result = await updateServerStatus({ maintenance_enabled: form.maintenance_enabled, message: form.message });
  saving.value = false;
  if (!result.ok) {
    error.value = result.error.message;
    return;
  }
  adopt(result.value);
  message.value = result.value.maintenance_enabled ? 'Saved: maintenance mode is on.' : 'Saved: NovaWorld is open.';
}

onMounted(load);
</script>
