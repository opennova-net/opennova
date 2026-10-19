<template>
  <section class="space-y-4" :aria-labelledby="`${idBase}-heading`">
    <h2 :id="`${idBase}-heading`" class="text-xl font-semibold text-ink">Sign-in</h2>

    <!-- Live regions, always in the page so a screen reader hears what lands in
         them, each its own box while it holds something. -->
    <div role="status" :class="message ? 'rounded-panel border border-online/40 bg-online/10 p-4 text-sm text-online' : 'sr-only'">{{ message }}</div>
    <div role="alert" :class="error ? 'rounded-panel border border-danger/40 bg-danger/10 p-4 text-sm text-danger' : 'sr-only'">{{ error }}</div>

    <div v-if="loading && !loaded" class="flex h-24 items-center justify-center rounded-panel border border-border bg-panel text-ink-muted">
      Loading the service's status…
    </div>

    <template v-else-if="loaded">
      <p
        class="novaworld-state flex items-center gap-3 rounded-panel border p-4 text-sm"
        :class="saved.maintenance_enabled ? 'border-warn/40 bg-warn/10 text-warn' : 'border-online/40 bg-online/10 text-online'"
      >
        <span :class="toneBadgeClass(saved.maintenance_enabled ? 'warn' : 'ok')">{{ saved.maintenance_enabled ? 'Maintenance' : 'Open' }}</span>
        <span class="font-medium">
          {{ saved.maintenance_enabled ? 'Maintenance mode is on: game clients cannot sign in.' : 'NovaWorld is open: game clients can sign in.' }}
        </span>
      </p>

      <form class="space-y-5 rounded-panel border border-border bg-panel p-6" @submit.prevent="save">
        <h3 class="sr-only">Maintenance mode</h3>
        <label class="flex items-center gap-3">
          <input
            v-model="form.maintenance_enabled"
            type="checkbox"
            :aria-describedby="`${idBase}-maintenance-hint`"
            class="h-4 w-4 rounded border-border bg-surface text-accent focus:ring-accent"
          />
          <span class="text-sm font-medium text-ink">Maintenance mode</span>
        </label>
        <p :id="`${idBase}-maintenance-hint`" class="text-xs text-ink-muted">
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

        <div class="flex items-center justify-end gap-3">
          <p v-if="dirty" class="text-xs text-warn">Not saved yet.</p>
          <button type="submit" :class="primaryButton" :disabled="saving || loading">{{ saving ? 'Saving…' : 'Save' }}</button>
        </div>
      </form>
    </template>
  </section>
</template>

<script setup lang="ts">
// The NovaWorld service's sign-in (GET/PUT /api/admin/server-status), on the
// NovaWorld page: whether game clients can sign in now, in a word and a
// colour, then maintenance mode and the message game clients see while it is
// on, with an unsaved change marked. refresh() reads it again (the page's
// Refresh).
import { computed, onMounted, reactive, ref } from 'vue';
import { fetchServerStatus, updateServerStatus } from '../api/admin';
import type { ServerStatus } from '../types/admin';
import { toneBadgeClass } from '../utils/infraView';

withDefaults(defineProps<{ idBase?: string }>(), { idBase: 'sign-in' });

const primaryButton =
  'rounded-control bg-accent px-4 py-2 text-sm font-medium text-on-accent transition hover:bg-accent/90 disabled:cursor-not-allowed disabled:opacity-60';

const loading = ref(true);
const loaded = ref(false);
const saving = ref(false);
const message = ref('');
const error = ref('');

const form = reactive<ServerStatus>({ maintenance_enabled: false, message: '' });
// The status as the server last answered it (the banner says that, not the
// unsaved form).
const saved = reactive<ServerStatus>({ maintenance_enabled: false, message: '' });
const dirty = computed(() => form.maintenance_enabled !== saved.maintenance_enabled || form.message !== saved.message);

function adopt(status: ServerStatus) {
  Object.assign(form, status);
  Object.assign(saved, status);
}

// A read again keeps an edit not saved yet: the banner follows the server, the
// form what the admin typed.
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
  message.value = result.value.maintenance_enabled ? 'Saved: maintenance mode is on.' : 'Saved: NovaWorld is open.';
}

onMounted(load);

defineExpose({ refresh: load, loading });
</script>
