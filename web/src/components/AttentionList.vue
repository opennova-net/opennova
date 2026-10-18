<template>
  <section class="space-y-3" :aria-labelledby="`${idBase}-heading`">
    <div class="flex flex-col gap-2 sm:flex-row sm:items-center sm:justify-between">
      <component :is="`h${headingLevel}`" :id="`${idBase}-heading`" class="text-xl font-semibold text-ink">Needs you</component>
      <div class="flex items-center gap-3">
        <p v-if="state.attention.value?.checked_at" class="text-xs text-ink-muted">
          Checked <RelativeTime :value="state.attention.value.checked_at" :now="now" />
        </p>
        <button type="button" :class="secondaryButton" :disabled="state.loading.value" @click="state.refresh">
          {{ state.loading.value ? 'Checking…' : 'Check again' }}
        </button>
      </div>
    </div>

    <p v-if="state.error.value" class="attention-error rounded-panel border border-danger/40 bg-danger/10 p-3 text-sm text-danger">
      What needs you could not be read: {{ state.error.value }}
    </p>
    <p v-else-if="!attention" class="text-sm text-ink-muted">Checking what needs you…</p>

    <template v-if="attention">
      <p v-if="needing === 0" class="attention-empty rounded-panel border border-online/40 bg-online/10 p-3 text-sm text-online">
        Nothing needs you: every machine reports and every server is running or starting on time.
      </p>

      <ul v-if="attention.findings.length" class="space-y-2">
        <li
          v-for="(f, index) in attention.findings"
          :key="index"
          class="attention-finding flex flex-col gap-2 rounded-panel border p-3 text-sm sm:flex-row sm:items-start sm:justify-between"
          :class="LEVEL_BOX[f.level]"
        >
          <div class="min-w-0 space-y-1">
            <p class="flex flex-wrap items-center gap-2">
              <span :class="toneBadgeClass(LEVEL_TONE[f.level])">{{ LEVEL_WORDS[f.level] }}</span>
              <span v-if="f.subject" class="font-semibold text-ink">{{ f.subject }}</span>
            </p>
            <p class="break-words text-ink">{{ f.text }}</p>
            <p v-if="f.since && sinceText(f.since)" class="text-xs text-ink-muted">For {{ sinceText(f.since) }}</p>
          </div>
          <RouterLink v-if="isPagePath(f.link)" :to="f.link!" class="shrink-0 text-accent hover:text-ink-bright">
            Open<span class="sr-only"> {{ f.subject || 'it' }}</span>
          </RouterLink>
          <span v-else-if="f.link" class="shrink-0 break-all font-mono text-xs text-ink-muted">{{ f.link }}</span>
        </li>
      </ul>

      <div v-if="attention.alarms.length" class="space-y-2">
        <h3 class="text-sm font-semibold text-ink">Firing alarms</h3>
        <ul class="space-y-2">
          <li
            v-for="a in attention.alarms"
            :key="a.name"
            class="attention-alarm space-y-1 rounded-panel border border-danger/40 bg-danger/10 p-3 text-sm"
          >
            <p class="flex flex-wrap items-center gap-2">
              <span :class="toneBadgeClass('bad')">Alarm</span>
              <span class="break-all font-mono text-xs text-ink">{{ a.name }}</span>
            </p>
            <p v-if="a.description" class="break-words text-ink">{{ a.description }}</p>
            <p v-if="a.state_since && sinceText(a.state_since)" class="text-xs text-ink-muted">Firing for {{ sinceText(a.state_since) }}</p>
          </li>
        </ul>
      </div>

      <p v-if="!attention.alarms_read" class="attention-no-alarms text-xs text-ink-muted">Alarms are not read here (local stack).</p>
    </template>
  </section>
</template>

<script setup lang="ts">
// What needs an admin (GET /api/admin/attention), on the Overview and the
// Fleet pages: the fleet's findings, danger first, each with its level in
// words and colour, what it is about, since when, and the page to look at;
// then the deployment's firing alarms. With none of either it says that
// nothing needs anyone; where alarms are not read (the local stack) it says so
// rather than implying none fire. The read is the admin layout's, shared with
// the nav's count (composables/useAttention.ts).
import { computed, ref, watch } from 'vue';
import { RouterLink } from 'vue-router';
import RelativeTime from './RelativeTime.vue';
import { attentionCount } from '../api/attention';
import { useAttention } from '../composables/useAttention';
import type { FindingLevel } from '../types/attention';
import { formatDuration, timeMs } from '../utils/format';
import { toneBadgeClass, type Tone } from '../utils/infraView';

withDefaults(defineProps<{ idBase?: string; headingLevel?: 2 | 3 }>(), { idBase: 'attention', headingLevel: 2 });

const LEVEL_WORDS: Record<FindingLevel, string> = { danger: 'Act now', warn: 'Look soon', info: 'Note' };
const LEVEL_TONE: Record<FindingLevel, Tone> = { danger: 'bad', warn: 'warn', info: 'idle' };
const LEVEL_BOX: Record<FindingLevel, string> = {
  danger: 'border-danger/40 bg-danger/10',
  warn: 'border-warn/40 bg-warn/10',
  info: 'border-border bg-panel',
};

const secondaryButton =
  'rounded-control border border-border bg-raised px-4 py-2 text-sm font-medium text-ink transition hover:bg-hover disabled:cursor-not-allowed disabled:opacity-40';

const state = useAttention();
const attention = computed(() => state.attention.value);
const needing = computed(() => (attention.value ? attentionCount(attention.value) : 0));
const now = ref(Date.now());
watch(state.loadedAt, () => (now.value = Date.now()));

// A site page the finding names (a path); anything else is shown as text.
function isPagePath(link: string | null): boolean {
  return !!link && link.startsWith('/') && !link.startsWith('//');
}

// "10 minutes": how long it has held; '' when unreadable.
function sinceText(since: string): string {
  const t = timeMs(since);
  return Number.isNaN(t) ? '' : formatDuration(Math.max(0, now.value - t));
}
</script>
