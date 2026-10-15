<template>
  <fieldset class="min-w-0">
    <legend class="sr-only">{{ legend }}</legend>
    <div class="inline-flex flex-wrap gap-1 rounded-panel border border-border bg-surface p-1">
      <label v-for="option in OPTIONS" :key="option.value" class="relative">
        <input
          :id="`${baseId}-${option.value}`"
          type="radio"
          class="peer sr-only"
          :name="baseId"
          :value="option.value"
          :checked="modelValue === option.value"
          :disabled="disabled"
          @change="emit('update:modelValue', option.value)"
        />
        <span
          class="block cursor-pointer rounded-control px-3 py-1.5 text-sm transition peer-checked:bg-raised peer-checked:font-semibold peer-checked:text-ink peer-focus-visible:ring-2 peer-focus-visible:ring-accent peer-disabled:cursor-not-allowed text-ink-muted hover:text-ink"
        >
          {{ option.label }}
        </span>
      </label>
    </div>
  </fieldset>
</template>

<script setup lang="ts">
// The stats' time range (the dataviz skill's date-range control, as presets):
// the last hour, day, week or month, one row above the charts it scopes. A
// radio group, so the arrow keys move it.
import type { StatsRange } from '../../types/hostedStats';

withDefaults(
  defineProps<{
    modelValue: StatsRange;
    // The base of the inputs' ids and their group's name.
    baseId: string;
    legend?: string;
    disabled?: boolean;
  }>(),
  { legend: 'Time range', disabled: false },
);

const emit = defineEmits<{
  (e: 'update:modelValue', value: StatsRange): void;
}>();

const OPTIONS: { value: StatsRange; label: string }[] = [
  { value: '1h', label: 'Last hour' },
  { value: '24h', label: 'Last 24 hours' },
  { value: '7d', label: 'Last 7 days' },
  { value: '30d', label: 'Last 30 days' },
];
</script>
