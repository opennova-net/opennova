import { flushPromises, mount } from '@vue/test-utils';
import { describe, expect, it, vi } from 'vitest';
import { computed } from 'vue';
import LoginPage from './LoginPage.vue';
import { testRouter } from '../test/router';

vi.mock('../composables/useSession', () => ({
  useSession: () => ({
    user: computed(() => null),
    ready: computed(() => true),
    signIn: vi.fn(),
    signOut: vi.fn(),
  }),
}));

describe('LoginPage', () => {
  it('offers the password reset beside the form', async () => {
    const router = await testRouter('/login');
    const wrapper = mount(LoginPage, { global: { plugins: [router] } });
    await flushPromises();
    const forgot = wrapper.findAll('a').find((a) => a.text() === 'Forgot password?');
    expect(forgot?.attributes('href')).toBe('/forgot');
    expect(wrapper.find('form').exists()).toBe(true);
  });
});
