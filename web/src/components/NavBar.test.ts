import { flushPromises, mount } from '@vue/test-utils';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import NavBar from './NavBar.vue';
import { testRouter } from '../test/router';
import type { SessionUser } from '../api/session';

const session = vi.hoisted(() => ({ user: null as SessionUser | null, signOut: vi.fn() }));

vi.mock('../composables/useSession', async () => {
  const { computed, ref } = await import('vue');
  return {
    useSession: () => ({
      user: computed(() => session.user),
      ready: ref(true),
      isAdmin: computed(() => session.user?.role === 'admin'),
      signOut: session.signOut,
    }),
  };
});

async function mountAt(path = '/') {
  const router = await testRouter(path);
  return mount(NavBar, { global: { plugins: [router] } });
}

function links(wrapper: Awaited<ReturnType<typeof mountAt>>) {
  return Object.fromEntries(wrapper.findAll('a').map((a) => [a.text(), a.attributes('href')]));
}

describe('NavBar', () => {
  beforeEach(() => {
    session.user = null;
    session.signOut.mockReset().mockResolvedValue({ ok: true, value: null });
  });

  it('shows the site links, Download, and Log in / Register to a visitor', async () => {
    const found = links(await mountAt('/'));
    expect(found['Play']).toBe('/play');
    expect(found['Servers']).toBe('/lobby');
    expect(found['Hosting']).toBe('/hosting');
    // GitHub is the footer's (SiteFooter.vue), not the header's.
    expect(found['GitHub']).toBeUndefined();
    expect(found['Download']).toBe('/downloads');
    expect(found['Log in']).toBe('/login');
    expect(found['Register']).toBe('/register');
    expect(found['My account']).toBeUndefined();
    expect(found['Admin']).toBeUndefined();
  });

  it('brings a visitor back to the page they log in from', async () => {
    expect(links(await mountAt('/lobby'))['Log in']).toBe('/login?redirect=/lobby');
  });

  it("links a logged-in user's name to their account, and Admin for an admin", async () => {
    session.user = { id: 1, username: 'ranger', role: 'admin' } as SessionUser;
    const wrapper = await mountAt('/');
    const found = links(wrapper);
    expect(found['Your account: logged in as ranger']).toBe('/me');
    expect(found['Admin']).toBe('/admin');
    expect(found['Log in']).toBeUndefined();
    expect(wrapper.text()).toContain('ranger');
    expect(wrapper.findAll('button').some((b) => b.text() === 'Log out')).toBe(true);
  });

  it('gives the admin workspace a compact header with a way back to the website', async () => {
    session.user = { id: 1, username: 'ranger', role: 'admin' };
    const wrapper = await mountAt('/admin');
    expect(wrapper.text()).toContain('Admin workspace');
    expect(wrapper.find('nav[aria-label="Primary"]').exists()).toBe(false);
    expect(links(wrapper)['Download']).toBeUndefined();
    expect(wrapper.findAll('a').some(a => a.text().startsWith('View website') && a.attributes('href') === '/')).toBe(true);
  });

  it('keeps the session when leaving a protected form is canceled', async () => {
    session.user = { id: 1, username: 'ranger', role: 'admin' };
    const router = await testRouter('/admin/products/JO');
    let allowLeave = false;
    router.beforeEach(() => allowLeave);
    const wrapper = mount(NavBar, { global: { plugins: [router] } });
    const logout = wrapper.findAll('button').find(button => button.text() === 'Log out')!;
    await logout.trigger('click');
    await flushPromises();
    expect(session.signOut).not.toHaveBeenCalled();
    expect(router.currentRoute.value.path).toBe('/admin/products/JO');
    expect(logout.attributes('disabled')).toBeUndefined();

    allowLeave = true;
    session.signOut.mockImplementation(async () => {
      expect(router.currentRoute.value.path).toBe('/');
      return { ok: true, value: null };
    });
    await logout.trigger('click');
    await flushPromises();
    expect(session.signOut).toHaveBeenCalledOnce();
    wrapper.unmount();
  });
});

describe('the phone menu', () => {
  beforeEach(() => {
    session.user = null;
  });

  it('opens from its toggle and closes on Escape', async () => {
    const wrapper = await mountAt('/');
    const toggle = wrapper.get('button[aria-controls="primary-navigation"]');
    expect(wrapper.find('#primary-navigation').exists()).toBe(false);

    await toggle.trigger('click');
    expect(toggle.attributes('aria-expanded')).toBe('true');
    expect(wrapper.find('#primary-navigation').exists()).toBe(true);

    await wrapper.get('#primary-navigation a').trigger('keydown', { key: 'Escape' });
    expect(toggle.attributes('aria-expanded')).toBe('false');
    expect(wrapper.find('#primary-navigation').exists()).toBe(false);
  });
});
