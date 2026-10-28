<template>
  <section class="space-y-3" aria-labelledby="attention-heading">
    <div class="flex items-center justify-between gap-3">
      <h2 id="attention-heading" class="section-heading">Needs you</h2>
      <button type="button" class="btn-ghost btn-sm" :disabled="state.loading.value" @click="state.refresh">
        {{ state.loading.value ? 'Checking…' : 'Check again' }}
      </button>
    </div>

    <Notice v-if="state.error.value" tone="bad" class="attention-error">Could not read what needs you: {{ state.error.value }}</Notice>
    <p v-else-if="!attention" class="text-sm text-ink-muted">Checking…</p>

    <template v-if="attention">
      <!-- A failing read: the last answer, as of its time, and never that nothing needs anyone. -->
      <p v-if="stale" class="attention-stale text-sm text-warn">
        As of <RelativeTime :value="state.loadedAt.value!" :now="now" />.
      </p>
      <p v-else-if="needing === 0" class="attention-empty text-sm text-online">Nothing needs you.</p>

      <ul v-if="attention.findings.length || attention.alarms.length" class="divide-y divide-border overflow-hidden rounded-panel border border-border bg-panel">
        <li v-for="(f, index) in attention.findings" :key="`f${index}`" class="attention-finding flex flex-wrap items-start gap-x-3 gap-y-1 px-4 py-3 text-sm">
          <span :class="badgeClass(LEVEL_TONE[f.level])">{{ LEVEL_WORDS[f.level] }}</span>
          <div class="min-w-0 flex-1">
            <RouterLink v-if="isPagePath(f.link)" :to="f.link!" class="entity-link font-medium">{{ f.subject || 'Open' }}</RouterLink>
            <span v-else class="font-medium text-ink">{{ f.subject }}</span>
            <span class="text-ink"> {{ f.text }}</span>
            <span v-if="f.link && !isPagePath(f.link)" class="block break-all font-mono text-xs text-ink-muted">{{ f.link }}</span>
          </div>
          <span v-if="f.since && sinceText(f.since)" class="shrink-0 text-xs text-ink-muted">{{ sinceText(f.since) }}</span>
        </li>
        <li v-for="a in attention.alarms" :key="a.name" class="attention-alarm flex flex-wrap items-start gap-x-3 gap-y-1 px-4 py-3 text-sm">
          <span :class="badgeClass('bad')">Alarm</span>
          <div class="min-w-0 flex-1">
            <RouterLink :to="alarmDestination(a.name).to" class="entity-link break-all font-mono text-xs">{{ a.name }}</RouterLink>
            <span v-if="a.description" class="text-ink"> {{ a.description }}</span>
          </div>
          <span v-if="a.state_since && sinceText(a.state_since)" class="shrink-0 text-xs text-ink-muted">{{ sinceText(a.state_since) }}</span>
        </li>
      </ul>
    </template>
  </section>
</template>

<script setup lang="ts">
// What needs an admin (GET /api/admin/attention), on the Overview: the
// fleet's findings, danger first, each with its level, what it is about
// (linked to the page to look at), what is wrong and for how long; then the
// deployment's firing alarms. A failed read shows why, and the last answer
// as of its time, never that nothing needs anyone. The read is the admin
// layout's, shared with the nav's count (composables/useAttention.ts).
import { computed, ref, watch } from 'vue';
import { RouterLink } from 'vue-router';
import RelativeTime from './RelativeTime.vue';
import Notice from './ui/Notice.vue';
import { attentionCount } from '../api/attention';
import { useAttention } from '../composables/useAttention';
import { alarmDestination } from '../utils/alarmLinks';
import type { FindingLevel } from '../types/attention';
import { formatDuration, timeMs } from '../utils/format';
import { badgeClass, type Tone } from '../utils/tone';

const LEVEL_WORDS: Record<FindingLevel, string> = { danger: 'Act now', warn: 'Look soon', info: 'Note' };
const LEVEL_TONE: Record<FindingLevel, Tone> = { danger: 'bad', warn: 'warn', info: 'muted' };

const state = useAttention();
const attention = computed(() => state.attention.value);
const needing = computed(() => (attention.value ? attentionCount(attention.value) : 0));
// The last answer while the read fails.
const stale = computed(() => !!state.error.value && attention.value !== null && state.loadedAt.value !== null);
const now = ref(Date.now());
watch([state.loadedAt, state.failedAt], () => (now.value = Date.now()));

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
