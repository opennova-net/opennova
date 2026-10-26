import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import ConfirmDialog from './ConfirmDialog.vue';
import { confirmAction, confirmation, settleConfirmation } from '../composables/useConfirm';

beforeEach(() => {
  // jsdom does not implement the native dialog methods.
  HTMLDialogElement.prototype.showModal = vi.fn(function (this: HTMLDialogElement) { this.open = true; });
  HTMLDialogElement.prototype.close = vi.fn(function (this: HTMLDialogElement) { this.open = false; });
});

afterEach(() => {
  settleConfirmation(false);
  document.body.innerHTML = '';
  vi.restoreAllMocks();
});

describe('action confirmation', () => {
  it('reviews the impact, defaults focus to Cancel, and restores focus on acceptance', async () => {
    const opener = document.createElement('button');
    document.body.append(opener);
    opener.focus();
    const wrapper = mount(ConfirmDialog, { attachTo: document.body });
    const result = confirmAction('The players on this server will disconnect.', 'Restart server');
    await flushPromises();
    const dialog = document.querySelector('dialog')!;
    expect(dialog.open).toBe(true);
    expect(dialog.textContent).toContain('The players on this server will disconnect.');
    expect(document.activeElement?.textContent).toBe('Cancel');
    (dialog.querySelectorAll('button')[1] as HTMLButtonElement).click();
    await expect(result).resolves.toBe(true);
    await flushPromises();
    expect(dialog.open).toBe(false);
    expect(document.activeElement).toBe(opener);
    wrapper.unmount();
  });

  it('Escape cancels without performing the action', async () => {
    const wrapper = mount(ConfirmDialog, { attachTo: document.body });
    const result = confirmAction('Stop this server?');
    await flushPromises();
    document.querySelector('dialog')!.dispatchEvent(new Event('cancel', { cancelable: true }));
    await expect(result).resolves.toBe(false);
    expect(confirmation.value).toBeNull();
    wrapper.unmount();
  });

  it('rejects a second request and cancels pending work on unmount', async () => {
    const wrapper = mount(ConfirmDialog);
    const first = confirmAction('First action');
    await expect(confirmAction('Second action')).resolves.toBe(false);
    expect(confirmation.value?.message).toBe('First action');
    wrapper.unmount();
    await expect(first).resolves.toBe(false);
  });
});
