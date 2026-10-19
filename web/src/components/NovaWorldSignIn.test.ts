import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import NovaWorldSignIn from './NovaWorldSignIn.vue';
import { fetchServerStatus, updateServerStatus } from '../api/admin';

vi.mock('../api/admin', () => ({ fetchServerStatus: vi.fn(), updateServerStatus: vi.fn() }));

const read = vi.mocked(fetchServerStatus);
const update = vi.mocked(updateServerStatus);

describe('NovaWorldSignIn', () => {
  let wrapper: ReturnType<typeof mount> | null = null;

  async function mountSignIn() {
    wrapper = mount(NovaWorldSignIn);
    await flushPromises();
    return wrapper;
  }

  beforeEach(() => {
    vi.clearAllMocks();
    read.mockResolvedValue({ ok: true, value: { maintenance_enabled: false, message: '' } });
  });

  afterEach(() => {
    wrapper?.unmount();
    wrapper = null;
  });

  it('says whether game clients can sign in, in a word beside the colour', async () => {
    const open = await mountSignIn();
    const state = open.get('.novaworld-state');
    expect(state.text()).toBe('OpenNovaWorld is open: game clients can sign in.');
    expect(state.classes()).toContain('border-online/40');
    open.unmount();
    read.mockResolvedValue({ ok: true, value: { maintenance_enabled: true, message: 'Back at noon' } });
    const closed = await mountSignIn();
    expect(closed.get('.novaworld-state').text()).toBe('MaintenanceMaintenance mode is on: game clients cannot sign in.');
    expect(closed.get('.novaworld-state').classes()).toContain('border-warn/40');
    expect((closed.get('textarea').element as HTMLTextAreaElement).value).toBe('Back at noon');
  });

  it('turns maintenance mode on, and the banner follows what the server saved', async () => {
    update.mockResolvedValue({ ok: true, value: { maintenance_enabled: true, message: 'Back soon' } });
    const page = await mountSignIn();
    await page.get('input[type="checkbox"]').setValue(true);
    await page.get('textarea').setValue('Back soon');
    // Not saved yet: marked, and the banner still says what the server has.
    expect(page.text()).toContain('Not saved yet.');
    expect(page.get('.novaworld-state').text()).toContain('NovaWorld is open');
    await page.get('form').trigger('submit');
    await flushPromises();
    expect(update).toHaveBeenCalledWith({ maintenance_enabled: true, message: 'Back soon' });
    expect(page.get('[role="status"]').text()).toBe('Saved: maintenance mode is on.');
    expect(page.get('.novaworld-state').text()).toContain('Maintenance mode is on');
    expect(page.text()).not.toContain('Not saved yet.');
  });

  it('keeps an unsaved edit when read again', async () => {
    const page = await mountSignIn();
    await page.get('textarea').setValue('Draft text');
    read.mockResolvedValue({ ok: true, value: { maintenance_enabled: true, message: 'Set elsewhere' } });
    await (page.vm as unknown as { refresh: () => Promise<void> }).refresh();
    await flushPromises();
    expect((page.get('textarea').element as HTMLTextAreaElement).value).toBe('Draft text');
    expect(page.get('.novaworld-state').text()).toContain('Maintenance mode is on');
  });

  it("shows the server's refusal", async () => {
    read.mockResolvedValue({ ok: false, error: { code: 'network', status: null, message: 'Could not reach the server.' } });
    const page = await mountSignIn();
    expect(page.get('[role="alert"]').text()).toBe('Could not reach the server.');
    expect(page.find('form').exists()).toBe(false);
  });
});
