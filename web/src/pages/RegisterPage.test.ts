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

async function mountPage() {
  const router = await testRouter('/register');
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

  it('requires the email, sends it, and says where the welcome mail went', async () => {
    register.mockResolvedValue(user(7, { username: 'newplayer', email: 'new@example.com' }));
    const wrapper = await mountPage();
    expect(wrapper.get('input[type="email"]').attributes('required')).toBeDefined();
    await submit(wrapper, ' New@Example.com ');
    expect(register).toHaveBeenCalledWith({ username: 'newplayer', password: 'secret', nwhandle: undefined, email: 'New@Example.com' });
    expect(wrapper.get('[role="status"]').text()).toContain('A welcome mail is on its way to new@example.com.');
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

  it("puts the server's refusal of the email under the field, any other in the alert", async () => {
    const wrapper = await mountPage();
    register.mockRejectedValueOnce(refusal({ error: 'email_exists', message: 'another account uses that email address' }));
    await submit(wrapper, 'taken@example.com');
    expect(wrapper.get('#register-email-hint').text()).toBe('Another account already uses that email address.');
    expect(wrapper.find('[role="alert"]').exists()).toBe(false);

    register.mockRejectedValueOnce(refusal({ error: 'username_exists', message: 'username already taken' }));
    await submit(wrapper, 'free@example.com');
    expect(wrapper.get('[role="alert"]').text()).toBe('Another account already has that username.');
    expect(wrapper.get('#register-email-hint').text()).toContain('We send a welcome mail here');
    wrapper.unmount();
  });
});
