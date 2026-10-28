<template>
  <section class="space-y-3" aria-labelledby="needs-you-heading">
    <h2 id="needs-you-heading" class="section-heading">Needs you</h2>
    <p v-if="items === null" class="text-sm text-ink-muted">Checking…</p>
    <p v-else-if="items.length === 0 && empty" class="needs-you-empty text-sm text-online">{{ empty }}</p>
    <ul v-if="items && items.length" class="divide-y divide-border overflow-hidden rounded-panel border border-border bg-panel">
      <li v-for="item in items" :key="item.key" class="needs-you-item flex items-center gap-3 px-4 py-3 text-sm">
        <Badge :tone="item.tone" class="shrink-0">{{ item.label }}</Badge>
        <div class="min-w-0 flex-1 truncate" :title="item.text">
          <ServerLink v-if="item.server" :name="item.server.name" :id="item.server.id" scope="owner" />
          <RouterLink v-else :to="item.to" class="entity-link font-medium">{{ item.subject }}</RouterLink>
          <span class="needs-you-text text-ink"> {{ item.text }}</span>
        </div>
        <span v-if="item.since" class="shrink-0 text-xs text-ink-muted"><RelativeTime :value="item.since" :now="now" /></span>
      </li>
    </ul>
  </section>
</template>

<script lang="ts">
import type { Tone } from '../utils/tone';

// One thing that needs the reader: a badge (its tone and word), what it is
// about (a server, else a named page), what is wrong, and since when.
export interface NeedsYouItem {
  key: string;
  tone: Tone;
  label: string;
  subject: string;
  to: string;
  // A hosted server of the reader's: named through ServerLink instead.
  server?: { id: number; name: string };
  text: string;
  since?: string | null;
}
</script>

<script setup lang="ts">
// "Needs you", the admin Overview's look for any list of things to act on: a
// row each, worst first as given, each one line. `items` null is not read yet;
// `empty` is said for none ('' says nothing, when a read failed).
import { RouterLink } from 'vue-router';
import RelativeTime from './RelativeTime.vue';
import ServerLink from './links/ServerLink.vue';
import Badge from './ui/Badge.vue';

withDefaults(defineProps<{ items: NeedsYouItem[] | null; now: number; empty?: string }>(), { empty: 'Nothing needs you.' });
</script>
