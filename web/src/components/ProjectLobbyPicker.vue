<template>
  <fieldset class="min-w-0 space-y-3" :disabled="disabled">
    <legend class="field-label">Lobby</legend>
    <div class="flex flex-wrap gap-x-6 gap-y-2">
      <label class="flex items-center gap-2 text-sm text-ink">
        <input v-model="mode" type="radio" :name="`${prefix}-lobby-mode`" value="new" class="accent-accent" />
        New lobby
      </label>
      <label class="flex items-center gap-2 text-sm text-ink">
        <input v-model="mode" type="radio" :name="`${prefix}-lobby-mode`" value="existing" class="accent-accent" />
        Existing lobby
      </label>
    </div>
    <div v-if="mode === 'new'">
      <label :for="`${prefix}-lobby-name`" class="field-label">Lobby name</label>
      <input :id="`${prefix}-lobby-name`" v-model="newName" type="text" autocomplete="off" :maxlength="NAME_MAX"
        class="field" :aria-invalid="error ? 'true' : undefined" :aria-describedby="`${prefix}-lobby-hint`" @input="$emit('nameInput')" />
    </div>
    <div v-else>
      <label :for="`${prefix}-lobby`" class="field-label">Existing lobby</label>
      <select :id="`${prefix}-lobby`" v-model="slug" class="field" :aria-invalid="error ? 'true' : undefined" :aria-describedby="`${prefix}-lobby-hint`">
        <option value="" disabled>{{ options.length ? 'Pick a lobby' : 'No lobbies for this protocol' }}</option>
        <option v-for="lobby in options" :key="lobby.slug" :value="lobby.slug">{{ lobby.display_name }}</option>
      </select>
    </div>
    <p :id="`${prefix}-lobby-hint`" class="text-xs" :class="error ? 'text-danger' : 'text-ink-muted'">
      {{ error || 'Players find this game’s servers in this lobby. The selection locks when the first release build starts.' }}
    </p>
  </fieldset>
</template>

<script setup lang="ts">
import type { LobbyOption } from '../types/products';
import { NAME_MAX } from '../utils/projectsView';

defineProps<{ prefix: string; options: LobbyOption[]; disabled?: boolean; error?: string }>();
defineEmits<{ nameInput: [] }>();
const mode = defineModel<'new' | 'existing'>('mode', { required: true });
const newName = defineModel<string>('newName', { required: true });
const slug = defineModel<string>('slug', { required: true });
</script>
