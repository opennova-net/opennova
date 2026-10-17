<template>
  <button
    v-if="count > 0"
    type="button"
    class="text-xs text-accent hover:text-ink-bright"
    :aria-pressed="modelValue ? 'true' : 'false'"
    @click="emit('update:modelValue', !modelValue)"
  >
    {{ modelValue ? 'Hide history' : `Show history (${count})` }}
  </button>
</template>

<script setup lang="ts">
// The rows a list hides by default (utils/history.ts), shown on request.
defineProps<{
  modelValue: boolean;
  // How many rows it hides.
  count: number;
}>();

const emit = defineEmits<{
  (e: 'update:modelValue', shown: boolean): void;
}>();
</script>
