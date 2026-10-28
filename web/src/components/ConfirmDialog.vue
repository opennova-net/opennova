<template>
  <Teleport to="body">
    <dialog
      ref="dialog"
      aria-labelledby="confirmation-title"
      aria-describedby="confirmation-message"
      class="m-auto w-[calc(100%-2rem)] max-w-lg rounded-panel border border-border-strong bg-panel p-0 text-ink shadow-2xl"
      @cancel.prevent="settleConfirmation(false)"
      @close="onClose"
    >
      <div v-if="confirmation" class="p-6">
        <h2 id="confirmation-title" class="section-heading">{{ confirmation.title }}</h2>
        <p id="confirmation-message" class="mt-3 whitespace-pre-line text-sm leading-relaxed text-ink">{{ confirmation.message }}</p>
        <div class="mt-6 flex justify-end gap-3">
          <button ref="cancel" type="button" class="btn-secondary" autofocus @click="settleConfirmation(false)">Cancel</button>
          <button type="button" class="btn-primary" @click="settleConfirmation(true)">Confirm</button>
        </div>
      </div>
    </dialog>
  </Teleport>
</template>

<script setup lang="ts">
// The site's one confirmation (composables/useConfirm.ts): the action's title,
// its one-sentence consequence, Cancel and Confirm.
import { nextTick, onBeforeUnmount, ref, watch } from 'vue';
import { confirmation, settleConfirmation } from '../composables/useConfirm';

const dialog = ref<HTMLDialogElement | null>(null);
const cancel = ref<HTMLButtonElement | null>(null);
let opener: HTMLElement | null = null;

function onClose() {
  // A previous dialog's close event may arrive after another review opened.
  if (!dialog.value?.open) settleConfirmation(false);
}

watch(confirmation, async (value) => {
  if (value) {
    opener = document.activeElement instanceof HTMLElement ? document.activeElement : null;
    await nextTick();
    if (!confirmation.value) return;
    dialog.value?.showModal();
    cancel.value?.focus();
  } else {
    dialog.value?.close();
    if (opener?.isConnected) opener.focus({ preventScroll: true });
    opener = null;
  }
});

onBeforeUnmount(() => settleConfirmation(false));
</script>
