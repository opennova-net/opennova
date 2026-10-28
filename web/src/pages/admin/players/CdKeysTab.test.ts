import * as actionReview from '../../../composables/useConfirm';
import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi, type MockInstance } from 'vitest';
import CdKeysTab from './CdKeysTab.vue';
import {
  activateCdKey,
  assignCdKey,
  fetchAccountCdKeys,
  fetchCdKeys,
  fetchKeyActivations,
  generateCdKeys,
  revokeCdKey,
} from '../../../api/cdKeys';
import { listUsers } from '../../../api/users';
import { testRouter } from '../../../test/router';
import type { CdKey, CdKeyActivation, CdKeyListResponse } from '../../../types/cdKeys';

vi.mock('../../../api/cdKeys', () => ({
  activateCdKey: vi.fn(),
  assignCdKey: vi.fn(),
  fetchAccountCdKeys: vi.fn(),
  fetchCdKeys: vi.fn(),
  fetchKeyActivations: vi.fn(),
  generateCdKeys: vi.fn(),
  revokeCdKey: vi.fn(),
}));
vi.mock('../../../api/users', () => ({ listUsers: vi.fn() }));

const fetchKeys = vi.mocked(fetchCdKeys);
const generate = vi.mocked(generateCdKeys);
const assign = vi.mocked(assignCdKey);
const revoke = vi.mocked(revokeCdKey);
const activations = vi.mocked(fetchKeyActivations);
const activate = vi.mocked(activateCdKey);
const account = vi.mocked(fetchAccountCdKeys);
const users = vi.mocked(listUsers);

function key(id: number, overrides: Partial<CdKey> = {}): CdKey {
  return {
    id,
    title: 'dfx2',
    serial: `DFX2-0000-0000-0000-000${id}`,
    wire_key: 'BMCCDKDKDLFFNGFDJHMFMMALJCLEFPAD',
    state: 'unassigned',
    account: null,
    created_by: 'admin root',
    created_at: '2026-10-14 12:00:00',
    assigned_at: null,
    revoked_at: null,
    activations: 0,
    ...overrides,
  };
}

function listOf(keys: CdKey[], overrides: Partial<CdKeyListResponse> = {}): CdKeyListResponse {
  return { total: keys.length, limit: 50, offset: 0, titles: ['dfx2'], auto_mint: true, keys, ...overrides };
}

const ASSIGNED = key(2, { state: 'assigned', account: { id: 7, username: 'pilot' }, assigned_at: '2026-10-14 12:05:00', activations: 1 });

const ACTIVATION: CdKeyActivation = {
  id: 1,
  key_id: 2,
  title: 'dfx2',
  serial: ASSIGNED.serial,
  nwpssk: 'ABCDEFGHIJKLMNOPQRSTUVW',
  nwact: 'DQNWWKWD',
  created_at: '2026-10-14 12:06:00',
};

async function mountTab(path = '/admin/players/cd-keys') {
  const router = await testRouter(path);
  const wrapper = mount(CdKeysTab, { global: { plugins: [router] } });
  await flushPromises();
  return { wrapper, router };
}

describe('CdKeysTab', () => {
  let confirm: MockInstance<typeof actionReview.confirmAction>;

  beforeEach(() => {
    vi.clearAllMocks();
    confirm = vi.spyOn(actionReview, 'confirmAction').mockResolvedValue(true);
    fetchKeys.mockResolvedValue({ ok: true, value: listOf([ASSIGNED, key(1), key(3, { state: 'revoked', created_by: 'auto' })]) });
  });

  afterEach(() => confirm.mockRestore());

  it('lists the keys with their game, state, account and machines', async () => {
    const { wrapper } = await mountTab();
    expect(fetchKeys).toHaveBeenCalledWith({ title: '', state: '', q: '', limit: 50, offset: 0 });
    expect(wrapper.get('h1').text()).toBe('Players');
    expect(wrapper.get('.cd-total').text()).toBe('3 keys');
    expect(wrapper.get('.cd-mint').text()).toBe('Auto-mint on');
    expect(wrapper.findAll('thead th').map((th) => th.text())).toEqual(['Key', 'State', 'Account', 'Machines', 'Made', 'Actions']);
    const [assigned, free, revoked] = wrapper.findAll('tr.cd-key-row');
    expect(assigned.text()).toContain('DFX2-0000-0000-0000-0002');
    expect(assigned.text()).toContain('Delta Force Xtreme 2');
    expect(assigned.get('.badge').text()).toBe('Assigned');
    // The account is its player page.
    expect(assigned.get('.player-link').attributes('href')).toBe('/admin/players/7');
    expect(assigned.text()).toContain('by root');
    expect(free.get('.badge').text()).toBe('Unassigned');
    expect(free.find('#cd-assign-1').exists()).toBe(true);
    expect(revoked.text()).toContain('at its first activation');
    expect(revoked.findAll('button').some((b) => b.text().startsWith('Revoke'))).toBe(false);
    expect(free.findAll('button').some((b) => b.text().startsWith('Revoke'))).toBe(true);
    expect(wrapper.text()).not.toContain('—');
  });

  it('filters in the URL query and reads the keys again for each change', async () => {
    const { wrapper, router } = await mountTab('/admin/players/cd-keys?state=assigned');
    expect(fetchKeys).toHaveBeenLastCalledWith({ title: '', state: 'assigned', q: '', limit: 50, offset: 0 });
    await wrapper.get('#cd-filter-state').setValue('revoked');
    await flushPromises();
    expect(fetchKeys).toHaveBeenLastCalledWith({ title: '', state: 'revoked', q: '', limit: 50, offset: 0 });
    await wrapper.get('#cd-filter-q').setValue(' pilot ');
    await wrapper.get('#cd-filter-q').element.closest('form')!.dispatchEvent(new Event('submit'));
    await flushPromises();
    expect(router.currentRoute.value.query).toEqual({ state: 'revoked', q: 'pilot' });
    expect(fetchKeys).toHaveBeenLastCalledWith({ title: '', state: 'revoked', q: 'pilot', limit: 50, offset: 0 });
    await wrapper.get('#cd-filter-title').setValue('dfx2');
    await flushPromises();
    expect(fetchKeys).toHaveBeenLastCalledWith({ title: 'dfx2', state: 'revoked', q: 'pilot', limit: 50, offset: 0 });
  });

  it('pages through the keys', async () => {
    fetchKeys.mockResolvedValue({ ok: true, value: listOf([key(1)], { total: 120 }) });
    const { wrapper, router } = await mountTab();
    const pages = wrapper.get('nav[aria-label="Key pages"]');
    expect(pages.text()).toContain('1 to 50 of 120');
    await pages.findAll('button').find((b) => b.text() === 'Next')!.trigger('click');
    await flushPromises();
    expect(router.currentRoute.value.query.page).toBe('2');
    expect(fetchKeys).toHaveBeenLastCalledWith({ title: '', state: '', q: '', limit: 50, offset: 50 });
  });

  it('generates keys from the Generate keys form, refusing a count out of range', async () => {
    const { wrapper } = await mountTab();
    expect(wrapper.find('#cd-generate').exists()).toBe(false);
    await wrapper.findAll('button').find((b) => b.text() === 'Generate keys')!.trigger('click');
    await wrapper.get('#cd-generate-count').setValue(500);
    await wrapper.get('#cd-generate').trigger('submit');
    await flushPromises();
    expect(generate).not.toHaveBeenCalled();
    expect(wrapper.get('#cd-generate-error').text()).toBe('Enter a whole number from 1 to 100.');

    generate.mockResolvedValue({ ok: true, value: [key(4), key(5)] });
    await wrapper.get('#cd-generate-count').setValue(2);
    await wrapper.get('#cd-generate').trigger('submit');
    await flushPromises();
    expect(generate).toHaveBeenCalledWith('dfx2', 2);
    expect(wrapper.get('#cd-generated').text()).toContain('DFX2-0000-0000-0000-0004');
    expect(wrapper.get('[role="status"]').text()).toBe('Generated 2 keys.');
    expect(fetchKeys).toHaveBeenCalledTimes(2);
  });

  it('assigns a key by account name and shows a refusal', async () => {
    const { wrapper } = await mountTab();
    assign.mockResolvedValueOnce({
      ok: false,
      error: { code: 'account_has_key', status: 409, message: 'That account already holds a key for this game. Revoke it first.' },
    });
    await wrapper.get('#cd-assign-1').setValue('pilot');
    await wrapper.get('#cd-assign-1').element.closest('form')!.dispatchEvent(new Event('submit'));
    await flushPromises();
    expect(assign).toHaveBeenCalledWith(1, 'pilot');
    expect(wrapper.get('[role="alert"]').text()).toContain('already holds a key');

    assign.mockResolvedValueOnce({ ok: true, value: key(1, { state: 'assigned', account: { id: 8, username: 'rookie' } }) });
    await wrapper.get('#cd-assign-1').setValue('rookie');
    await wrapper.get('#cd-assign-1').element.closest('form')!.dispatchEvent(new Event('submit'));
    await flushPromises();
    expect(assign).toHaveBeenLastCalledWith(1, 'rookie');
    expect(wrapper.get('[role="status"]').text()).toContain("now on rookie's account");
  });

  it('revokes a key after a confirmation', async () => {
    revoke.mockResolvedValue({ ok: true, value: { ...ASSIGNED, state: 'revoked', revoked_at: '2026-10-14 13:00:00' } });
    const { wrapper } = await mountTab();
    const button = wrapper.findAll('tr.cd-key-row')[0].findAll('button').find((b) => b.text().startsWith('Revoke'))!;
    confirm.mockResolvedValueOnce(false);
    await button.trigger('click');
    await flushPromises();
    expect(revoke).not.toHaveBeenCalled();
    await button.trigger('click');
    await flushPromises();
    expect(confirm).toHaveBeenLastCalledWith(`Revoke key ${ASSIGNED.serial} (pilot's)? It never activates a machine again.`);
    expect(revoke).toHaveBeenCalledWith(2);
    expect(wrapper.get('[role="status"]').text()).toContain('is revoked');
  });

  it("opens a key's machines under its row and activates one by hand", async () => {
    activations.mockResolvedValue({ ok: true, value: [ACTIVATION] });
    const { wrapper } = await mountTab();
    const machines = wrapper.findAll('tr.cd-key-row')[0].get('button.cd-machines');
    await machines.trigger('click');
    await flushPromises();
    expect(activations).toHaveBeenCalledWith(2);
    expect(machines.attributes('aria-expanded')).toBe('true');
    // The machines are the row after the key's.
    const detail = wrapper.findAll('tbody > tr')[1];
    expect(detail.classes()).toContain('cd-key-detail');
    expect(detail.get('#cd-key-panel').text()).toContain('ABCDEFGHIJKLMNOPQRSTUVW');
    expect(detail.get('table').classes()).toContain('data-table');

    activate.mockResolvedValue({ ok: true, value: { nwact: 'NWACTVALUE', reactivated: false, key: ASSIGNED } });
    await wrapper.get('#cd-activate-nwpssk').setValue('tttttttttttttttttttttt');
    expect(wrapper.get('#cd-activate-nwpssk').element.closest('form')!.querySelector('button')!.disabled).toBe(true);
    await wrapper.get('#cd-activate-nwpssk').setValue('ttttttttttttttttttttttt');
    await wrapper.get('#cd-activate-nwpssk').element.closest('form')!.dispatchEvent(new Event('submit'));
    await flushPromises();
    expect(activate).toHaveBeenCalledWith(2, 'TTTTTTTTTTTTTTTTTTTTTTT');
    expect(wrapper.get('#cd-activate-nwact').text()).toBe('NWACTVALUE');

    // A second click closes them.
    await wrapper.findAll('tr.cd-key-row')[0].get('button.cd-machines').trigger('click');
    expect(wrapper.find('#cd-key-panel').exists()).toBe(false);
  });

  it("opens the account a player page links to, without reading every account", async () => {
    account.mockResolvedValue({ ok: true, value: { account: { id: 7, username: 'pilot' }, keys: [ASSIGNED], activations: [ACTIVATION] } });
    const { wrapper } = await mountTab('/admin/players/cd-keys?account=7');
    expect(account).toHaveBeenCalledWith(7);
    expect(users).not.toHaveBeenCalled();
    const panel = wrapper.get('#cd-account-panel');
    expect(panel.get('.player-link').attributes('href')).toBe('/admin/players/7');
    expect(panel.text()).toContain(ASSIGNED.serial);
    expect(panel.text()).toContain('ABCDEFGHIJKLMNOPQRSTUVW');
  });

  it('looks an account up by name', async () => {
    users.mockResolvedValue({ ok: true, value: [{ id: 7, username: 'Pilot' } as never] });
    account.mockResolvedValue({ ok: true, value: { account: { id: 7, username: 'Pilot' }, keys: [ASSIGNED], activations: [] } });
    const { wrapper, router } = await mountTab();
    await wrapper.get('#cd-account-name').setValue('pilot');
    await wrapper.get('#cd-account-name').element.closest('form')!.dispatchEvent(new Event('submit'));
    await flushPromises();
    expect(router.currentRoute.value.query.account).toBe('7');
    expect(account).toHaveBeenCalledWith(7);
    const panel = wrapper.get('#cd-account-panel');
    expect(panel.text()).toContain(ASSIGNED.serial);
    expect(panel.text()).toContain('No machine is activated on this account.');

    users.mockResolvedValue({ ok: true, value: [] });
    await wrapper.get('#cd-account-name').setValue('ghost');
    await wrapper.get('#cd-account-name').element.closest('form')!.dispatchEvent(new Event('submit'));
    await flushPromises();
    expect(wrapper.get('[role="alert"]').text()).toBe('No player account is named ghost.');
    expect(wrapper.find('#cd-account-panel').exists()).toBe(false);
  });
});
