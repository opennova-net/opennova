<template>
  <tr class="table-state">
    <td :colspan="colspan" class="px-4 py-6 text-center text-ink-muted">
      {{ rows === null ? (loading ? 'Loading…' : 'Not loaded.') : empty }}
    </td>
  </tr>
</template>

<script setup lang="ts">
// The one row a table shows instead of rows: loading, not read, or nothing to
// show. `rows` is null before the first read; the parent shows this only when
// there is no row to draw.
defineProps<{ colspan: number; rows: unknown[] | null; loading?: boolean; empty: string }>();
</script>
