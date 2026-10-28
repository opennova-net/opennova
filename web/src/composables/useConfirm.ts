import { readonly, shallowRef } from 'vue';

interface Confirmation {
  title: string;
  message: string;
  resolve: (accepted: boolean) => void;
}

const pending = shallowRef<Confirmation | null>(null);
export const confirmation = readonly(pending);

// One action can be reviewed at a time. A second click cannot replace the
// action the user is already reviewing or leave a promise unresolved.
export function confirmAction(message: string, title = 'Confirm'): Promise<boolean> {
  if (pending.value) return Promise.resolve(false);
  return new Promise((resolve) => {
    pending.value = { title, message, resolve };
  });
}

export function settleConfirmation(accepted: boolean): void {
  const action = pending.value;
  pending.value = null;
  action?.resolve(accepted);
}
