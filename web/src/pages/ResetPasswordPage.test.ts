import { flushPromises, mount } from '@vue/test-utils';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import ResetPasswordPage from './ResetPasswordPage.vue';
import { resetPassword } from '../api/passwordReset';
import { refreshSession } from '../composables/useSession';
import { testRouter } from '../test/router';

vi.mock('../api/passwordReset', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../api/passwordReset')>()),
  resetPassword: vi.fn(),
}));
vi.mock('../composables/useSession', () => {
  const refresh = vi.fn().mockResolvedValue({ ok: true, value: null });
  return { refreshSession: refresh, useSession: () => ({ refresh }) };
});

const reset = vi.mocked(resetPassword);
const TOKEN = '0f'.repeat(32);

async function mountPage(path: string) {
  const router = await testRouter(path);
  const wrapper = mount(ResetPasswordPage, { global: { plugins: [router] }, attachTo: document.body });
  await flushPromises();
  return wrapper;
}

async function fill(wrapper: Awaited<ReturnType<typeof mountPage>>, first: string, second: string) {
  const inputs = wrapper.findAll('input[type="password"]');
  await inputs[0].setValue(first);
  await inputs[1].setValue(second);
  await wrapper.get('form').trigger('submit');
  await flushPromises();
}

describe('ResetPasswordPage', () => {
  beforeEach(() => vi.clearAllMocks());

  it('sets the new password with the link token, then offers the login', async () => {
    reset.mockResolvedValue({ ok: true });
    const wrapper = await mountPage(`/reset?token=${TOKEN}`);
    await fill(wrapper, 'new-secret', 'new-secret');
    expect(reset).toHaveBeenCalledWith(TOKEN, 'new-secret');
    expect(wrapper.get('[role="status"]').text()).toContain('Your new password is set.');
    expect(wrapper.findAll('a').map((a) => a.attributes('href'))).toContain('/login');
    expect(vi.mocked(refreshSession)).toHaveBeenCalled();
    expect(wrapper.text()).not.toContain('—');
    wrapper.unmount();
  });

  it('asks for the password twice, the same', async () => {
    const wrapper = await mountPage(`/reset?token=${TOKEN}`);
    await fill(wrapper, 'new-secret', 'new-secrte');
    expect(reset).not.toHaveBeenCalled();
    expect(wrapper.get('[role="alert"]').text()).toBe('The two passwords do not match.');
    expect(wrapper.findAll('input[type="password"]')[1].attributes('aria-invalid')).toBe('true');
    wrapper.unmount();
  });

  it('says a spent or expired link is dead and offers a new one', async () => {
    reset.mockResolvedValue({
      ok: false,
      error: { code: 'invalid_token', status: 400, message: 'This link has expired or was used already. Ask for a new one.' },
    });
    const wrapper = await mountPage(`/reset?token=${TOKEN}`);
    await fill(wrapper, 'new-secret', 'new-secret');
    expect(wrapper.get('[role="alert"]').text()).toContain('This link has expired or was used already.');
    expect(wrapper.find('form').exists()).toBe(false);
    expect(wrapper.findAll('a').map((a) => a.attributes('href'))).toContain('/forgot');
    wrapper.unmount();
  });

  it('knows a link with no token or a malformed one at once', async () => {
    for (const path of ['/reset', '/reset?token=nothex', `/reset?token=${TOKEN.toUpperCase()}`]) {
      const wrapper = await mountPage(path);
      expect(wrapper.find('form').exists()).toBe(false);
      expect(wrapper.get('[role="alert"]').text()).toContain('This link has expired or was used already.');
      wrapper.unmount();
    }
    expect(reset).not.toHaveBeenCalled();
  });

  it('shows any other refusal in words and keeps the form', async () => {
    reset.mockResolvedValue({
      ok: false,
      error: { code: 'network', status: null, message: 'Could not reach the server. Check your connection and try again.' },
    });
    const wrapper = await mountPage(`/reset?token=${TOKEN}`);
    await fill(wrapper, 'new-secret', 'new-secret');
    expect(wrapper.get('[role="alert"]').text()).toBe('Could not reach the server. Check your connection and try again.');
    expect(wrapper.find('form').exists()).toBe(true);
    wrapper.unmount();
  });
});
