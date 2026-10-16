import { mount } from '@vue/test-utils';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import NavBar from './NavBar.vue';
import { testRouter } from '../test/router';
import type { SessionUser } from '../api/session';

const session = vi.hoisted(() => ({ user: null as SessionUser | null }));

vi.mock('../composables/useSession', async () => {
  const { computed, ref } = await import('vue');
  return {
    useSession: () => ({
      user: computed(() => session.user),
      ready: ref(true),
      isAdmin: computed(() => session.user?.role === 'admin'),
      signOut: async () => ({ ok: true, value: null }),
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
  });

  it('shows the site links, Download, and Log in / Register to a visitor', async () => {
    const found = links(await mountAt('/'));
    expect(found['Lobbies']).toBe('/lobby');
    expect(found['Support tiers']).toBe('/support');
    expect(found['GitHub']).toBe('https://github.com/opennova-net/opennova');
    expect(found['Download']).toBe('/#downloads');
    expect(found['Log in']).toBe('/login');
    expect(found['Register']).toBe('/register');
    expect(found['My servers']).toBeUndefined();
    expect(found['Admin']).toBeUndefined();
  });

  it('brings a visitor back to the page they log in from', async () => {
    expect(links(await mountAt('/lobby'))['Log in']).toBe('/login?redirect=/lobby');
  });

  it('shows My servers and the account to a logged-in user, and Admin to an admin', async () => {
    session.user = { id: 1, username: 'ranger', role: 'admin' } as SessionUser;
    const wrapper = await mountAt('/');
    const found = links(wrapper);
    expect(found['My servers']).toBe('/me/servers');
    expect(found['Admin']).toBe('/admin');
    expect(found['Log in']).toBeUndefined();
    expect(wrapper.text()).toContain('ranger');
    expect(wrapper.findAll('button').some((b) => b.text() === 'Log out')).toBe(true);
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
