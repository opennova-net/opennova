import { flushPromises, mount } from '@vue/test-utils';
import { describe, expect, it, vi } from 'vitest';
import AccountLayout from './AccountLayout.vue';
import { testRouter } from '../../test/router';
import { refreshSession } from '../../composables/useSession';

vi.mock('../../api/session', async (original) => ({
  ...(await original<typeof import('../../api/session')>()),
  me: vi.fn(async () => ({ ok: true, value: { id: 7, username: 'biggy', role: 'player' } })),
}));

describe('AccountLayout', () => {
  it('highlights only Overview at the account root, Billing on billing and a project under Projects', async () => {
    const router = await testRouter('/me');
    const page = mount(AccountLayout, { global: { plugins: [router] } });
    const nav = page.get('nav[aria-label="Your account"]');
    expect(nav.findAll('a').map((a) => a.text())).toEqual(['Overview', 'Servers', 'Projects', 'Billing', 'Account']);
    expect(nav.findAll('[aria-current="page"]').map((a) => a.text())).toEqual(['Overview']);
    await router.push('/me/billing');
    expect(nav.findAll('[aria-current="page"]').map((a) => a.text())).toEqual(['Billing']);
    expect(nav.get('a[aria-current="page"]').attributes('href')).toBe('/me/billing');
    await router.push('/me/projects/NIGHTOPS');
    expect(nav.findAll('[aria-current="page"]').map((a) => a.text())).toEqual(['Projects']);
    page.unmount();
  });

  it('names the account above its sections, with no eyebrow', async () => {
    await refreshSession();
    const router = await testRouter('/me');
    const page = mount(AccountLayout, { global: { plugins: [router] } });
    expect(page.get('.account-user').text()).toBe('biggy');
    expect(page.text()).not.toContain('Your space');
    expect(page.text()).not.toContain('NovaWorld account');
    expect(page.get('a[href="/lobby"]').text()).toContain('Find a game');
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
    await router.push('/me/billing');
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
