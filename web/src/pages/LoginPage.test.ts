import { flushPromises, mount } from '@vue/test-utils';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import LoginPage from './LoginPage.vue';
import type { SessionUser } from '../api/session';
import { testRouter } from '../test/router';

const session = vi.hoisted(() => ({ user: null as SessionUser | null, signIn: vi.fn(), signOut: vi.fn() }));

vi.mock('../composables/useSession', async () => {
  const { computed } = await import('vue');
  return {
    useSession: () => ({
      user: computed(() => session.user),
      ready: computed(() => true),
      signIn: session.signIn,
      signOut: session.signOut,
    }),
  };
});

async function mountAt(path = '/login') {
  const router = await testRouter(path);
  const wrapper = mount(LoginPage, { global: { plugins: [router] }, attachTo: document.body });
  await flushPromises();
  return { wrapper, router };
}

describe('LoginPage', () => {
  beforeEach(() => {
    session.user = null;
    session.signIn.mockReset();
    session.signOut.mockReset();
  });

  it('shows the form in the shared card, with the reset and the registration below', async () => {
    const { wrapper } = await mountAt('/login?redirect=/lobby');
    expect(wrapper.get('h1').text()).toBe('Log in');
    expect(wrapper.find('form').exists()).toBe(true);
    expect(wrapper.get('label[for="login-username"]').text()).toBe('Username');
    expect(wrapper.get('label[for="login-password"]').text()).toBe('Password');
    const links = Object.fromEntries(wrapper.findAll('a').map((a) => [a.text(), a.attributes('href')]));
    expect(links['Forgot password?']).toBe('/forgot');
    expect(links['Register']).toBe('/register?redirect=/lobby');
    wrapper.unmount();
  });

  it('logs in and goes on to the page it came from; a refusal shows in words', async () => {
    session.signIn.mockResolvedValueOnce({ ok: false, error: { code: 'invalid_credentials', status: 401, message: 'Wrong username or password.' } });
    const { wrapper, router } = await mountAt('/login?redirect=/lobby');
    await wrapper.get('#login-username').setValue('ranger');
    await wrapper.get('#login-password').setValue('nope');
    await wrapper.get('form').trigger('submit');
    await flushPromises();
    expect(session.signIn).toHaveBeenCalledWith('ranger', 'nope');
    expect(wrapper.get('[role="alert"]').text()).toBe('Wrong username or password.');
    expect(wrapper.get('#login-password').attributes('aria-invalid')).toBe('true');

    session.signIn.mockResolvedValueOnce({ ok: true, value: { id: 1, username: 'ranger', role: 'player' } as SessionUser });
    await wrapper.get('#login-password').setValue('right');
    await wrapper.get('form').trigger('submit');
    await flushPromises();
    expect(router.currentRoute.value.fullPath).toBe('/lobby');
    wrapper.unmount();
  });

  it('tells a visitor already logged in who they are, with Continue and Log out', async () => {
    session.user = { id: 1, username: 'ranger', role: 'player' } as SessionUser;
    session.signOut.mockResolvedValue({ ok: true, value: null });
    const { wrapper } = await mountAt('/login?redirect=/me');
    expect(wrapper.find('form').exists()).toBe(false);
    expect(wrapper.text()).toContain('Logged in as ranger.');
    expect(wrapper.findAll('a').find((a) => a.text() === 'Continue')?.attributes('href')).toBe('/me');
    await wrapper.findAll('button').find((b) => b.text() === 'Log out')!.trigger('click');
    await flushPromises();
    expect(session.signOut).toHaveBeenCalledOnce();
    wrapper.unmount();
  });
});
