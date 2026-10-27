import { flushPromises, mount } from '@vue/test-utils';
import { describe, expect, it, vi } from 'vitest';
import AccountLayout from './AccountLayout.vue';
import { testRouter } from '../../test/router';

describe('AccountLayout', () => {
  it('highlights only Overview at the account root, keeps addresses under Servers and a project under Projects', async () => {
    const router = await testRouter('/me');
    const page = mount(AccountLayout, { global: { plugins: [router] } });
    const nav = page.get('nav[aria-label="Your account"]');
    expect(nav.findAll('a').map((a) => a.text())).toEqual(['Overview', 'Servers', 'Projects', 'Billing', 'Account']);
    expect(nav.findAll('[aria-current="page"]').map((a) => a.text())).toEqual(['Overview']);
    await router.push('/me/addresses');
    expect(nav.findAll('[aria-current="page"]').map((a) => a.text())).toEqual(['Servers']);
    await router.push('/me/projects/NIGHTOPS');
    expect(nav.findAll('[aria-current="page"]').map((a) => a.text())).toEqual(['Projects']);
    page.unmount();
  });

  it('opens a modal account menu and closes it after navigation', async () => {
    const router = await testRouter('/me');
    const page = mount(AccountLayout, { global: { plugins: [router] } });
    const dialog = page.get('dialog').element as HTMLDialogElement;
    dialog.showModal = vi.fn(() => { dialog.open = true; });
    dialog.close = vi.fn(() => { dialog.open = false; });
    await page.get('button[aria-controls="account-menu"]').trigger('click');
    expect(dialog.showModal).toHaveBeenCalledOnce();
    expect(dialog.open).toBe(true);
    await router.push('/me/support');
    await flushPromises();
    expect(dialog.close).toHaveBeenCalledOnce();
    expect(dialog.open).toBe(false);
    page.unmount();
  });

  it('closes the mobile drawer when the desktop navigation becomes available', async () => {
    const router = await testRouter('/me');
    const page = mount(AccountLayout, { global: { plugins: [router] } });
    const dialog = page.get('dialog').element as HTMLDialogElement;
    dialog.showModal = vi.fn(() => { dialog.open = true; });
    dialog.close = vi.fn(() => { dialog.open = false; });
    vi.stubGlobal('innerWidth', 600);
    await page.get('button[aria-controls="account-menu"]').trigger('click');
    window.dispatchEvent(new Event('resize'));
    expect(dialog.open).toBe(true);
    vi.stubGlobal('innerWidth', 1024);
    window.dispatchEvent(new Event('resize'));
    expect(dialog.close).toHaveBeenCalledOnce();
    expect(dialog.open).toBe(false);
    page.unmount();
    vi.unstubAllGlobals();
  });
});
