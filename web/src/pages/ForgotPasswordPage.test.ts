import { flushPromises, mount } from '@vue/test-utils';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import ForgotPasswordPage from './ForgotPasswordPage.vue';
import { requestPasswordReset } from '../api/passwordReset';
import { testRouter } from '../test/router';

vi.mock('../api/passwordReset', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../api/passwordReset')>()),
  requestPasswordReset: vi.fn(),
}));

const request = vi.mocked(requestPasswordReset);

async function mountPage() {
  const router = await testRouter('/forgot');
  const wrapper = mount(ForgotPasswordPage, { global: { plugins: [router] }, attachTo: document.body });
  await flushPromises();
  return wrapper;
}

describe('ForgotPasswordPage', () => {
  beforeEach(() => vi.clearAllMocks());

  it('sends the address and says the same thing whatever the server knows', async () => {
    request.mockResolvedValue({ ok: true });
    const wrapper = await mountPage();
    await wrapper.get('input[type="email"]').setValue('  Player@Example.com ');
    await wrapper.get('form').trigger('submit');
    await flushPromises();
    expect(request).toHaveBeenCalledWith('Player@Example.com');
    expect(wrapper.get('[role="status"]').text()).toContain('If that address has an account, we sent it a link');
    expect(wrapper.find('form').exists()).toBe(false);
    expect(wrapper.text()).not.toContain('—');
    wrapper.unmount();
  });

  it('checks the address before it asks, and shows the server refusal in words', async () => {
    const wrapper = await mountPage();
    await wrapper.get('input[type="email"]').setValue('player@localhost');
    await wrapper.get('form').trigger('submit');
    await flushPromises();
    expect(request).not.toHaveBeenCalled();
    expect(wrapper.get('[role="alert"]').text()).toBe('Enter an email address such as name@example.com.');
    expect(wrapper.get('input[type="email"]').attributes('aria-invalid')).toBe('true');

    request.mockResolvedValueOnce({
      ok: false,
      error: { code: 'rate_limited', status: 429, message: 'Too many requests. Try again in 20 minutes.', retryAfterSeconds: 1200 },
    });
    await wrapper.get('input[type="email"]').setValue('player@example.com');
    await wrapper.get('form').trigger('submit');
    await flushPromises();
    expect(wrapper.get('[role="alert"]').text()).toBe('Too many requests. Try again in 20 minutes.');
    wrapper.unmount();
  });

  it('links back to the login', async () => {
    const wrapper = await mountPage();
    expect(wrapper.findAll('a').map((a) => a.attributes('href'))).toContain('/login');
    wrapper.unmount();
  });
});
