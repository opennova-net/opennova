import { flushPromises, mount } from '@vue/test-utils';
import { AxiosError, AxiosHeaders, type AxiosResponse } from 'axios';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import RegisterPage from './RegisterPage.vue';
import { registerUser } from '../api/users';
import { user } from '../test/adminFixtures';
import { testRouter } from '../test/router';

vi.mock('../api/users', () => ({ registerUser: vi.fn() }));

const register = vi.mocked(registerUser);

function refusal(data: unknown): AxiosError {
  const config = { headers: new AxiosHeaders() };
  return new AxiosError('refused', 'ERR_BAD_REQUEST', config, null, {
    status: 400,
    statusText: '',
    data,
    headers: {},
    config,
  } as AxiosResponse);
}

async function mountPage(path = '/register') {
  const router = await testRouter(path);
  const wrapper = mount(RegisterPage, { global: { plugins: [router] }, attachTo: document.body });
  await flushPromises();
  return wrapper;
}

async function submit(wrapper: Awaited<ReturnType<typeof mountPage>>, email: string) {
  await wrapper.get('input[type="text"]').setValue('newplayer');
  await wrapper.get('input[type="email"]').setValue(email);
  await wrapper.get('input[type="password"]').setValue('secret');
  await wrapper.get('form').trigger('submit');
  await flushPromises();
}

describe('RegisterPage', () => {
  beforeEach(() => vi.clearAllMocks());

  it('requires the email and offers login without exposing internal account identifiers', async () => {
    register.mockResolvedValue(user(7, { username: 'newplayer', email: 'new@example.com' }));
    const wrapper = await mountPage();
    expect(wrapper.get('h1').text()).toBe('Register');
    expect(wrapper.get('button[type="submit"]').text()).toBe('Register');
    expect(wrapper.get('label[for="register-email"]').text()).toBe('Email');
    expect(wrapper.get('input[type="email"]').attributes('required')).toBeDefined();
    await submit(wrapper, ' New@Example.com ');
    expect(register).toHaveBeenCalledWith({ username: 'newplayer', password: 'secret', email: 'New@Example.com' });
    expect(wrapper.get('[role="status"]').text()).toContain('Account created for newplayer');
    expect(wrapper.get('[role="status"]').text()).not.toContain('PCID');
    expect(wrapper.get('[role="status"] a').attributes('href')).toContain('/login');
    expect(wrapper.text()).not.toContain('—');
    wrapper.unmount();
  });

  it('shows the address rule under the field before it sends anything', async () => {
    const wrapper = await mountPage();
    await submit(wrapper, 'new@localhost');
    expect(register).not.toHaveBeenCalled();
    expect(wrapper.get('#register-email-hint').text()).toBe('Enter an email address such as name@example.com.');
    expect(wrapper.get('input[type="email"]').attributes('aria-invalid')).toBe('true');
    await submit(wrapper, '');
    expect(register).not.toHaveBeenCalled();
    wrapper.unmount();
  });

  it('carries a selected tier through explicit login and rejects an external return address', async () => {
    register.mockResolvedValue(user(7, { username: 'newplayer', email: 'new@example.com' }));
    const page = await mountPage('/register?redirect=%2Fme%2Fbilling%3Ftier%3D2');
    await submit(page, 'new@example.com');
    const target = new URL(page.get('[role="status"] a').attributes('href')!, 'https://opennova.test');
    expect(target.pathname).toBe('/login');
    expect(target.searchParams.get('redirect')).toBe('/me/billing?tier=2');
    expect(target.searchParams.get('username')).toBe('newplayer');
    page.unmount();
    const external = await mountPage('/register?redirect=https%3A%2F%2Fevil.example');
    await submit(external, 'new@example.com');
    expect(external.get('[role="status"] a').attributes('href')).not.toContain('evil');
    external.unmount();
  });

  it("puts the server's refusal of the email under the field, any other in the alert", async () => {
    const wrapper = await mountPage();
    register.mockRejectedValueOnce(refusal({ error: 'email_exists', message: 'another account uses that email address' }));
    await submit(wrapper, 'taken@example.com');
    expect(wrapper.get('#register-email-hint').text()).toBe('Another account already uses that email address.');
    expect(wrapper.find('[role="alert"]').exists()).toBe(false);

    register.mockRejectedValueOnce(refusal({ error: 'username_exists', message: 'username already taken' }));
    await submit(wrapper, 'free@example.com');
    expect(wrapper.get('[role="alert"]').text()).toBe('Another account already has that username.');
    // The email field's error goes once the email is fine again.
    expect(wrapper.find('#register-email-hint').exists()).toBe(false);
    wrapper.unmount();
  });
});
