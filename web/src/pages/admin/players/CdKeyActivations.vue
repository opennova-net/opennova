<template>
  <p v-if="activations.length === 0" class="cd-activations-empty text-sm text-ink-muted">{{ empty }}</p>
  <div v-else class="table-wrap">
    <table class="cd-activations data-table">
      <caption class="sr-only">Machines activated</caption>
      <thead>
        <tr>
          <th scope="col">When</th>
          <th scope="col">Key</th>
          <th scope="col">Machine (NWPSSK)</th>
          <th scope="col">Code (NWACT)</th>
        </tr>
      </thead>
      <tbody>
        <tr v-for="a in activations" :key="a.id">
          <td class="whitespace-nowrap text-xs text-ink-muted"><RelativeTime :value="a.created_at" :now="now" /></td>
          <td class="whitespace-nowrap font-mono text-xs">{{ a.serial }}</td>
          <td class="font-mono text-xs">{{ a.nwpssk }}</td>
          <td class="max-w-md break-all font-mono text-xs text-ink-muted">{{ a.nwact }}</td>
        </tr>
      </tbody>
    </table>
  </div>
</template>

<script setup lang="ts">
// The machines CD keys activated: when, the key, the machine, its code.
import RelativeTime from '../../../components/RelativeTime.vue';
import type { CdKeyActivation } from '../../../types/cdKeys';

defineProps<{ activations: CdKeyActivation[]; empty: string; now: number }>();
</script>
