import { inject, onBeforeUnmount, onMounted } from 'vue';
import { matchedRouteKey, onBeforeRouteLeave, onBeforeRouteUpdate } from 'vue-router';
import { confirmAction } from './useConfirm';

// The form owns its dirty check, including clearing it after a successful save.
export function useUnsavedChanges(isDirty: () => boolean): void {
  const review = () => !isDirty() || confirmAction('Your unsaved changes will be lost. Leave this page?', 'Unsaved changes');
  // Some forms also render outside a route, including isolated component tests.
  if (inject(matchedRouteKey, undefined)?.value) {
    onBeforeRouteLeave(review);
    onBeforeRouteUpdate((to, from) => to.path === from.path || review());
  }

  function beforeUnload(event: BeforeUnloadEvent) {
    if (!isDirty()) return;
    event.preventDefault();
    event.returnValue = '';
  }
  onMounted(() => window.addEventListener('beforeunload', beforeUnload));
  onBeforeUnmount(() => window.removeEventListener('beforeunload', beforeUnload));
}
