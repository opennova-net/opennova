import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import SettingsSection from './SettingsSection.vue';
import { fetchSettings, saveSettings } from '../../api/settings';
import { serviceSettings } from '../../test/settingsFixtures';
import { testRouter } from '../../test/router';

vi.mock('../../api/settings', () => ({
  fetchSettings: vi.fn(),
  saveSettings: vi.fn(),
}));

const read = vi.mocked(fetchSettings);
const save = vi.mocked(saveSettings);

let wrapper: ReturnType<typeof mount> | null = null;

async function mountSection() {
  const router = await testRouter('/admin/settings');
  wrapper = mount(SettingsSection, { global: { plugins: [router] } });
  await flushPromises();
  return wrapper;
}

function saveButton() {
  return wrapper!.findAll('button').find((b) => b.text() === 'Save settings')!;
}

describe('SettingsSection', () => {
  beforeEach(() => {
    vi.clearAllMocks();
    read.mockResolvedValue({ ok: true, value: serviceSettings() });
  });

  afterEach(() => {
    wrapper?.unmount();
    wrapper = null;
  });

  it('shows the switch, the four maximums and the sold capacity', async () => {
    const page = await mountSection();
    expect((page.get('#settings-subscriptions-enabled').element as HTMLInputElement).checked).toBe(false);
    expect((page.get('#settings-max_shared').element as HTMLInputElement).value).toBe('2');
    expect((page.get('#settings-shared_slots').element as HTMLInputElement).value).toBe('12');
    expect((page.get('#settings-max_own').element as HTMLInputElement).value).toBe('2');
    expect((page.get('#settings-max_addresses').element as HTMLInputElement).value).toBe('4');
    expect(page.get('#settings-max_shared-hint').text()).toBe('Deployment default 2');
    expect(page.get('#settings-capacity').text()).toContain('Sold: 5 of 24 shared places');
    // Nothing changed: nothing to save.
    expect(saveButton().attributes('disabled')).toBeDefined();
  });

  it('saves the switch and a raised fleet together', async () => {
    const saved = serviceSettings({
      subscriptions_enabled: true,
      fleet: { max_shared: 5, shared_slots: 12, max_own: 2, max_addresses: 4 },
    });
    save.mockResolvedValue({ ok: true, value: saved });
    const page = await mountSection();
    await page.get('#settings-subscriptions-enabled').setValue(true);
    await page.get('#settings-max_shared').setValue(5);
    await page.get('form').trigger('submit');
    await flushPromises();
    expect(save).toHaveBeenCalledWith({ subscriptions_enabled: true, max_shared: 5, shared_slots: 12, max_own: 2, max_addresses: 4 });
    expect(page.get('[role="status"]').text()).toBe('Settings saved.');
    expect(saveButton().attributes('disabled')).toBeDefined();
  });

  it('refuses a value out of range before it is sent', async () => {
    const page = await mountSection();
    await page.get('#settings-shared_slots').setValue(0);
    expect(page.get('#settings-shared_slots-hint').text()).toBe('A whole number from 1 to 32');
    expect(saveButton().attributes('disabled')).toBeDefined();
    await page.get('form').trigger('submit');
    expect(save).not.toHaveBeenCalled();
  });

  it('keeps static addresses fixed where the deployment runs none', async () => {
    read.mockResolvedValue({
      ok: true,
      value: serviceSettings({
        addresses_available: false,
        fleet: { max_shared: 2, shared_slots: 12, max_own: 2, max_addresses: 0 },
        boot: { max_shared: 2, shared_slots: 12, max_own: 2, max_addresses: 0 },
      }),
    });
    const page = await mountSection();
    expect(page.get('#settings-max_addresses').attributes('disabled')).toBeDefined();
    expect(page.text()).toContain('This deployment runs no static addresses.');
  });

  it('says the refusal the server answered with', async () => {
    save.mockResolvedValue({ ok: false, error: { code: 'invalid_field', status: 400, message: 'The server refused a value.' } });
    const page = await mountSection();
    await page.get('#settings-max_own').setValue(3);
    await page.get('form').trigger('submit');
    await flushPromises();
    expect(page.get('[role="alert"]').text()).toBe('The server refused a value.');
  });
});
