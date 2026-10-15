<template>
  <section class="space-y-3" :aria-labelledby="id">
    <h2 :id="id" class="text-xl font-semibold text-ink">{{ heading }}</h2>
    <div class="space-y-2 rounded-panel border border-dashed border-border-strong bg-panel p-4 text-sm text-ink-muted">
      <p class="font-semibold text-ink">To be written by the operator:</p>
      <ul class="list-disc space-y-1 pl-5">
        <li v-for="(item, index) in covers" :key="index">{{ item }}</li>
      </ul>
    </div>
  </section>
</template>

<script setup lang="ts">
// One section of a legal page still to be written: its heading, and what the
// operator must cover in it, set apart as a placeholder.
defineProps<{
  id: string;
  heading: string;
  // What the section must say, one point each.
  covers: string[];
}>();
</script>
