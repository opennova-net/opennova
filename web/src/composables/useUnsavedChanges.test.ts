import { flushPromises, mount } from '@vue/test-utils';
import { defineComponent, h, ref } from 'vue';
import { createMemoryHistory, createRouter, isNavigationFailure, RouterView } from 'vue-router';
import { afterEach, describe, expect, it, vi } from 'vitest';
import * as confirmations from './useConfirm';
import { useUnsavedChanges } from './useUnsavedChanges';

afterEach(() => vi.restoreAllMocks());

describe('unsaved changes', () => {
  it('guards route exits and record changes, while allowing query tabs and saved forms', async () => {
    const dirty = ref(true);
    const review = vi.spyOn(confirmations, 'confirmAction').mockResolvedValue(false);
    const form = defineComponent({
      setup() {
        useUnsavedChanges(() => dirty.value);
        return () => h('form');
      },
    });
    const router = createRouter({
      history: createMemoryHistory(),
      routes: [
        { path: '/edit/:id', component: form },
        { path: '/', component: { render: () => null } },
      ],
    });
    await router.push('/edit/1');
    const wrapper = mount(RouterView, { global: { plugins: [router] } });
    await flushPromises();
    expect(isNavigationFailure(await router.push('/'))).toBe(true);
    expect(router.currentRoute.value.path).toBe('/edit/1');
    expect(isNavigationFailure(await router.push('/edit/2'))).toBe(true);
    expect(review).toHaveBeenCalledTimes(2);

    await router.push('/edit/1?tab=settings');
    expect(review).toHaveBeenCalledTimes(2);
    review.mockResolvedValueOnce(true);
    await router.push('/edit/2');
    expect(router.currentRoute.value.path).toBe('/edit/2');
    dirty.value = false;
    await router.push('/');
    expect(review).toHaveBeenCalledTimes(3);
    wrapper.unmount();
  });

  it('protects a dirty form from unload and removes its listener on unmount', () => {
    const dirty = ref(true);
    const wrapper = mount(defineComponent({
      setup() {
        useUnsavedChanges(() => dirty.value);
        return () => null;
      },
    }));
    const unload = () => {
      const event = new Event('beforeunload', { cancelable: true });
      window.dispatchEvent(event);
      return event.defaultPrevented;
    };
    expect(unload()).toBe(true);
    dirty.value = false;
    expect(unload()).toBe(false);
    dirty.value = true;
    wrapper.unmount();
    expect(unload()).toBe(false);
  });
});
