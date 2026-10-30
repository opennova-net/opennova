<template>
  <span v-if="build" class="build-text whitespace-nowrap">
    <span v-if="version" class="build-version font-semibold text-ink">{{ version }}</span>
    <template v-if="version && build.commit">{{ ' ' }}</template>
    <span v-if="build.commit" class="build-commit font-mono">{{ build.commit }}</span>
  </span>
  <span v-else class="text-ink-muted">{{ none }}</span>
</template>

<script setup lang="ts">
// A build in the words the maintainer reads it by: ONE's version ("ONE
// 0.0.22.5922") and its commit's short sha ("b0c2b38"), or the sha alone where no
// version is known (utils/buildView.ts).
import { computed } from 'vue';
import { versionText, type Build } from '../utils/buildView';

const props = withDefaults(defineProps<{ build: Build | null; none?: string }>(), { none: 'Not reported' });

const version = computed(() => (props.build ? versionText(props.build) : null));
</script>
