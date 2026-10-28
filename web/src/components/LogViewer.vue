<template>
  <section class="space-y-3" :aria-labelledby="`${idBase}-heading`">
    <div class="flex flex-wrap items-center justify-between gap-3">
      <div class="min-w-0">
        <component :is="`h${headingLevel}`" :id="`${idBase}-heading`" class="section-heading">{{ heading }}</component>
        <p class="log-source break-all text-xs text-ink-muted">{{ page ? logSourceText(page) : 'Reading…' }}</p>
      </div>
      <div class="flex flex-wrap items-center gap-2">
        <form class="flex items-center gap-2" role="search" @submit.prevent="applyFilter">
          <label :for="`${idBase}-filter`" class="sr-only">Lines holding</label>
          <input
            :id="`${idBase}-filter`"
            v-model="filterInput"
            type="search"
            autocomplete="off"
            spellcheck="false"
            placeholder="Filter lines"
            class="field w-48 font-mono"
          />
          <button type="submit" class="btn-secondary btn-sm">Filter</button>
          <button v-if="q" type="button" class="btn-ghost btn-sm" @click="clearFilter">Clear</button>
        </form>
        <label v-if="followable" class="flex items-center gap-2 text-sm text-ink" title="Every 5 seconds">
          <input :id="`${idBase}-follow`" v-model="following" type="checkbox" class="h-4 w-4 accent-accent" @change="onFollow" />
          Follow
        </label>
      </div>
    </div>

    <LiveNotices :error="error" :poll-trouble="pollTrouble" />
    <p v-if="gapNote" class="log-gap text-xs text-warn">{{ gapNote }}</p>

    <div class="rounded-panel border border-border bg-surface">
      <div class="flex items-center justify-between gap-3 border-b border-border px-3 py-2 text-xs text-ink-muted">
        <span>{{ countText }}</span>
        <button
          v-if="before"
          type="button"
          class="log-older btn-ghost btn-sm"
          :disabled="olderBusy"
          @click="loadOlder"
        >
          {{ olderBusy ? 'Loading…' : 'Load older lines' }}
        </button>
        <span v-else-if="page">Start of log</span>
      </div>
      <ol
        ref="box"
        class="max-h-[32rem] overflow-auto px-3 py-2 font-mono text-xs leading-relaxed text-ink"
        :aria-label="`${heading}: its lines, oldest first`"
        tabindex="0"
      >
        <li v-if="page && lines.length === 0" class="text-ink-muted">{{ q ? 'No line holds that text.' : 'No line yet.' }}</li>
        <li v-for="(line, index) in lines" :key="index" class="log-line whitespace-pre-wrap break-all">
          <time v-if="line.at" :datetime="line.at" class="text-ink-muted">{{ lineTime(line.at) }}</time>{{ line.at ? ` ${line.text}` : line.text }}
        </li>
      </ol>
    </div>
  </section>
</template>

<script setup lang="ts">
// One log on the site (GET /api/admin/logs): a hosted server's start, a
// machine's hostd, a program's tasks or one of them. Its newest 200 lines,
// oldest first; older ones load through the page's cursor; while following, the
// newest page is read every 5 s and its lines past the last one shown are
// added (scrolled to them when the view sat at the bottom). The filter is the
// route's `q`. Above, where the lines were read (CloudWatch's group and
// stream, or the local stack's directory).
import { computed, nextTick, ref, watch } from 'vue';
import LiveNotices from './ui/LiveNotices.vue';
import { fetchLogs } from '../api/logs';
import { usePolling } from '../composables/usePolling';
import type { LogLine, LogPage, LogQuery } from '../types/logs';
import { appendNewer, lineTime, logFilterProblem, logSourceText } from '../utils/logView';

const props = withDefaults(
  defineProps<{
    idBase: string;
    subject: LogQuery['subject'];
    id: string | number;
    generation?: number;
    task?: string;
    heading?: string;
    headingLevel?: 2 | 3;
    // Whether the log can still grow (a live server's, a running program's):
    // offer the follow, on to start with.
    followable?: boolean;
  }>(),
  { generation: undefined, task: undefined, heading: 'Log', headingLevel: 3, followable: true },
);

const PAGE = 200;
const FOLLOW_MS = 5_000;

const page = ref<LogPage | null>(null);
const lines = ref<LogLine[]>([]);
const before = ref<string | null>(null);
const q = ref('');
const filterInput = ref('');
const following = ref(props.followable);
const olderBusy = ref(false);
const error = ref('');
const pollTrouble = ref(false);
const gapNote = ref('');
const box = ref<HTMLElement | null>(null);

function base(): LogQuery {
  const query: LogQuery = { subject: props.subject, id: props.id, limit: PAGE };
  if (props.generation !== undefined) query.generation = props.generation;
  if (props.task) query.task = props.task;
  if (q.value) query.q = q.value;
  return query;
}

function atBottom(): boolean {
  const el = box.value;
  return !el || el.scrollTop + el.clientHeight >= el.scrollHeight - 4;
}

async function scrollToBottom() {
  await nextTick();
  const el = box.value;
  if (el) el.scrollTop = el.scrollHeight;
}

// The newest page: the first read, a filter's, or a follow's.
const poll = usePolling({
  async read(signal, background) {
    const result = await fetchLogs(base(), signal);
    if (signal.aborted) return false;
    if (!result.ok) {
      if (background && page.value !== null) pollTrouble.value = true;
      else error.value = result.error.message;
      return false;
    }
    error.value = '';
    pollTrouble.value = false;
    const fresh = result.value;
    const stick = atBottom();
    const first = page.value === null;
    const merged = first ? null : appendNewer(lines.value, fresh.lines);
    if (merged === null) {
      // The first page, or more came than a page holds: the page alone.
      if (!first) gapNote.value = 'Skipped to the newest lines; load older lines to read the ones between.';
      lines.value = [...fresh.lines];
      before.value = fresh.before;
    } else {
      lines.value = merged;
    }
    page.value = fresh;
    if (first || stick) await scrollToBottom();
    return true;
  },
  next: () => (following.value && props.followable ? FOLLOW_MS : null),
});

function startOver() {
  page.value = null;
  lines.value = [];
  before.value = null;
  gapNote.value = '';
  error.value = '';
  void poll.refresh();
}

async function loadOlder() {
  if (!before.value || olderBusy.value) return;
  olderBusy.value = true;
  const result = await fetchLogs({ ...base(), before: before.value });
  olderBusy.value = false;
  if (!result.ok) {
    error.value = result.error.message;
    return;
  }
  error.value = '';
  lines.value = [...result.value.lines, ...lines.value];
  before.value = result.value.before;
}

function applyFilter() {
  const text = filterInput.value.trim();
  const problem = logFilterProblem(text);
  if (problem) {
    error.value = problem;
    return;
  }
  q.value = text;
  startOver();
}

function clearFilter() {
  filterInput.value = '';
  q.value = '';
  startOver();
}

function onFollow() {
  if (following.value) void poll.poke();
}

// Another log in the same place (another start, another task): start over.
watch(() => [props.subject, props.id, props.generation, props.task], startOver);

const countText = computed(() => {
  if (!page.value) return '';
  const n = lines.value.length;
  return `${n} line${n === 1 ? '' : 's'}${q.value ? ` holding "${q.value}"` : ''}, oldest first`;
});
</script>
