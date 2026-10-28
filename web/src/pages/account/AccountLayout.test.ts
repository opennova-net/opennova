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
  it('lists four sections and marks the one a page is under', async () => {
    const router = await testRouter('/me');
    const page = mount(AccountLayout, { global: { plugins: [router] } });
    const nav = page.get('aside nav[aria-label="Your account"]');
    expect(nav.findAll('a').map((a) => a.text())).toEqual(['Overview', 'Servers', 'Projects', 'Billing']);
    expect(nav.findAll('[aria-current="page"]').map((a) => a.text())).toEqual(['Overview']);
    await router.push('/me/billing');
    expect(nav.findAll('[aria-current="page"]').map((a) => a.text())).toEqual(['Billing']);
    await router.push('/me/projects/NIGHTOPS');
    expect(nav.findAll('[aria-current="page"]').map((a) => a.text())).toEqual(['Projects']);
    await router.push('/me/servers/hosts/167772161');
    expect(nav.findAll('[aria-current="page"]').map((a) => a.text())).toEqual(['Servers']);
    page.unmount();
  });

  it('names who is logged in under the sections, with the way out', async () => {
    await refreshSession();
    const router = await testRouter('/me');
    const page = mount(AccountLayout, { global: { plugins: [router] } });
    const footer = page.get('aside .workspace-footer');
    expect(footer.get('.account-user').text()).toBe('biggy');
    expect(footer.get('a[href="/lobby"]').text()).toContain('Find a game');
    expect(footer.find('a[href="/admin"]').exists()).toBe(false);
    expect(footer.findAll('button').map((b) => b.text())).toEqual(['Log out']);
    page.unmount();
  });

  it('opens a modal drawer on a narrow screen and closes it after navigation', async () => {
    const router = await testRouter('/me');
    const page = mount(AccountLayout, { global: { plugins: [router] } });
    const dialog = page.get('dialog').element as HTMLDialogElement;
    dialog.showModal = vi.fn(() => { dialog.setAttribute('open', ''); });
    dialog.close = vi.fn(() => { dialog.removeAttribute('open'); dialog.dispatchEvent(new Event('close')); });
    await page.get('button[aria-controls^="workspace-drawer"]').trigger('click');
    await flushPromises();
    expect(dialog.showModal).toHaveBeenCalledOnce();
    expect(page.get('dialog nav').findAll('a')).toHaveLength(4);
    await router.push('/me/billing');
    await flushPromises();
    expect(dialog.close).toHaveBeenCalledOnce();
    page.unmount();
  });
});
