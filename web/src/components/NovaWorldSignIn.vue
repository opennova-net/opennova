<template>
  <section class="space-y-3" :aria-labelledby="`${idBase}-heading`">
    <h2 :id="`${idBase}-heading`" class="section-heading flex items-center gap-3">
      Sign-in
      <Badge v-if="loaded" class="novaworld-state" :tone="saved.maintenance_enabled ? 'warn' : 'ok'">{{ saved.maintenance_enabled ? 'Maintenance' : 'Open' }}</Badge>
    </h2>

    <LiveNotices :message="message" :error="error" />
    <p v-if="loading && !loaded" class="text-sm text-ink-muted">Loading…</p>

    <form v-if="loaded" class="card flex flex-col gap-3 p-4 sm:flex-row sm:items-end" @submit.prevent="save">
      <label class="flex shrink-0 items-center gap-2 text-sm text-ink sm:pb-2">
        <input v-model="form.maintenance_enabled" type="checkbox" class="h-4 w-4 rounded border-border bg-surface text-accent focus:ring-accent" />
        Maintenance mode
      </label>
      <div class="min-w-0 flex-1">
        <label :for="`${idBase}-message`" class="field-label">Message</label>
        <textarea
          :id="`${idBase}-message`"
          v-model="form.message"
          rows="1"
          placeholder="Shown to a game client signing in during maintenance"
          class="field"
        />
      </div>
      <div class="flex shrink-0 items-center gap-2">
        <span v-if="dirty" class="text-xs text-warn">Not saved</span>
        <button type="submit" class="btn-primary" :disabled="saving || loading">{{ saving ? 'Saving…' : 'Save' }}</button>
      </div>
    </form>
  </section>
</template>

<script setup lang="ts">
// NovaWorld's sign-in on the System page (GET/PUT /api/admin/server-status):
// open or in maintenance, and maintenance mode with the message game clients
// see. refresh() reads it again, keeping an edit not saved yet.
import { computed, onMounted, reactive, ref } from 'vue';
import Badge from './ui/Badge.vue';
import LiveNotices from './ui/LiveNotices.vue';
import { fetchServerStatus, updateServerStatus } from '../api/admin';
import type { ServerStatus } from '../types/admin';

withDefaults(defineProps<{ idBase?: string }>(), { idBase: 'sign-in' });

const loading = ref(true);
const loaded = ref(false);
const saving = ref(false);
const message = ref('');
const error = ref('');

const form = reactive<ServerStatus>({ maintenance_enabled: false, message: '' });
// The status as the server last answered it (the badge says that, not the
// unsaved form).
const saved = reactive<ServerStatus>({ maintenance_enabled: false, message: '' });
const dirty = computed(() => form.maintenance_enabled !== saved.maintenance_enabled || form.message !== saved.message);

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
  if (loaded.value && dirty.value) Object.assign(saved, result.value);
  else adopt(result.value);
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
  message.value = result.value.maintenance_enabled ? 'Maintenance mode is on.' : 'NovaWorld is open.';
}

onMounted(load);

defineExpose({ refresh: load, loading });
</script>
