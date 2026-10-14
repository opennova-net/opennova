import { flushPromises, mount } from '@vue/test-utils';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import CdKeysSection from './CdKeysSection.vue';
import {
  activateCdKey,
  assignCdKey,
  fetchAccountCdKeys,
  fetchCdKeys,
  fetchKeyActivations,
  generateCdKeys,
  revokeCdKey,
} from '../../api/cdKeys';
import { listUsers } from '../../api/users';
import type { CdKey, CdKeyListResponse } from '../../types/cdKeys';

vi.mock('../../api/cdKeys', () => ({
  activateCdKey: vi.fn(),
  assignCdKey: vi.fn(),
  fetchAccountCdKeys: vi.fn(),
  fetchCdKeys: vi.fn(),
  fetchKeyActivations: vi.fn(),
  generateCdKeys: vi.fn(),
  revokeCdKey: vi.fn(),
}));
vi.mock('../../api/users', () => ({ listUsers: vi.fn() }));

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

function listOf(keys: CdKey[], autoMint = true): CdKeyListResponse {
  return { total: keys.length, limit: 50, offset: 0, titles: ['dfx2'], auto_mint: autoMint, keys };
}

const ASSIGNED = key(2, {
  state: 'assigned',
  account: { id: 7, username: 'pilot' },
  assigned_at: '2026-10-14 12:05:00',
  activations: 1,
});

describe('CdKeysSection', () => {
  beforeEach(() => {
    vi.clearAllMocks();
    fetchKeys.mockResolvedValue({ ok: true, value: listOf([ASSIGNED, key(1)]) });
  });

  it('lists the keys with their state, account and machines', async () => {
    const wrapper = mount(CdKeysSection);
    await flushPromises();
    expect(fetchKeys).toHaveBeenCalledWith({ title: '', state: '', q: '', limit: 50, offset: 0 });
    const rows = wrapper.findAll('tr.cd-key-row');
    expect(rows).toHaveLength(2);
    expect(rows[0].text()).toContain('DFX2-0000-0000-0000-0002');
    expect(rows[0].text()).toContain('Assigned');
    expect(rows[0].text()).toContain('pilot');
    expect(rows[1].text()).toContain('Unassigned');
    expect(rows[1].find('#cd-assign-1').exists()).toBe(true);
    expect(wrapper.text()).toContain('An account with no key gets one at its first activation.');
    // No em dash in the page's copy.
    expect(wrapper.text()).not.toContain('—');
  });

  it('applies the filters', async () => {
    const wrapper = mount(CdKeysSection);
    await flushPromises();
    await wrapper.get('#cd-filter-state').setValue('revoked');
    await wrapper.get('#cd-filter-q').setValue(' pilot ');
    await wrapper.get('#cd-filter-title').setValue('dfx2');
    await wrapper.get('#cd-filter-q').element.closest('form')!.dispatchEvent(new Event('submit'));
    await flushPromises();
    expect(fetchKeys).toHaveBeenLastCalledWith({ title: 'dfx2', state: 'revoked', q: 'pilot', limit: 50, offset: 0 });
  });

  it('generates keys, refusing a count out of range', async () => {
    const wrapper = mount(CdKeysSection);
    await flushPromises();
    await wrapper.get('#cd-generate-count').setValue(500);
    await wrapper.get('#cd-generate-count').element.closest('form')!.dispatchEvent(new Event('submit'));
    await flushPromises();
    expect(generate).not.toHaveBeenCalled();
    expect(wrapper.get('#cd-generate-error').text()).toBe('Enter a whole number from 1 to 100.');

    generate.mockResolvedValue({ ok: true, value: [key(3), key(4)] });
    await wrapper.get('#cd-generate-count').setValue(2);
    await wrapper.get('#cd-generate-count').element.closest('form')!.dispatchEvent(new Event('submit'));
    await flushPromises();
    expect(generate).toHaveBeenCalledWith('dfx2', 2);
    expect(wrapper.get('#cd-generated').text()).toContain('DFX2-0000-0000-0000-0003');
    expect(wrapper.text()).toContain('Generated 2 keys.');
    expect(fetchKeys).toHaveBeenCalledTimes(2);
  });

  it('assigns a key by account name and shows a refusal', async () => {
    const wrapper = mount(CdKeysSection);
    await flushPromises();
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
    const confirm = vi.spyOn(window, 'confirm').mockReturnValue(true);
    revoke.mockResolvedValue({ ok: true, value: { ...ASSIGNED, state: 'revoked', revoked_at: '2026-10-14 13:00:00' } });
    const wrapper = mount(CdKeysSection);
    await flushPromises();
    const button = wrapper.findAll('tr.cd-key-row')[0].findAll('button').find((b) => b.text().startsWith('Revoke'));
    await button!.trigger('click');
    await flushPromises();
    expect(confirm).toHaveBeenCalled();
    expect(revoke).toHaveBeenCalledWith(2);
    expect(wrapper.get('[role="status"]').text()).toContain('is revoked');
    confirm.mockRestore();
  });

  it("shows a key's machines and activates one by hand", async () => {
    activations.mockResolvedValue({
      ok: true,
      value: [
        {
          id: 1,
          key_id: 2,
          title: 'dfx2',
          serial: ASSIGNED.serial,
          nwpssk: 'ABCDEFGHIJKLMNOPQRSTUVW',
          nwact: 'DQNWWKWD',
          created_at: '2026-10-14 12:06:00',
        },
      ],
    });
    const wrapper = mount(CdKeysSection);
    await flushPromises();
    const machines = wrapper.findAll('tr.cd-key-row')[0].findAll('button').find((b) => b.text().startsWith('1'));
    await machines!.trigger('click');
    await flushPromises();
    expect(activations).toHaveBeenCalledWith(2);
    expect(wrapper.get('#cd-key-panel').text()).toContain('ABCDEFGHIJKLMNOPQRSTUVW');

    activate.mockResolvedValue({ ok: true, value: { nwact: 'NWACTVALUE', reactivated: false, key: ASSIGNED } });
    await wrapper.get('#cd-activate-nwpssk').setValue('tttttttttttttttttttttt');
    expect(wrapper.get('#cd-activate-nwpssk').element.closest('form')!.querySelector('button')!.disabled).toBe(true);
    await wrapper.get('#cd-activate-nwpssk').setValue('ttttttttttttttttttttttt');
    await wrapper.get('#cd-activate-nwpssk').element.closest('form')!.dispatchEvent(new Event('submit'));
    await flushPromises();
    expect(activate).toHaveBeenCalledWith(2, 'TTTTTTTTTTTTTTTTTTTTTTT');
    expect(wrapper.get('#cd-activate-nwact').text()).toBe('NWACTVALUE');
  });

  it("looks an account up by name and shows its keys and machines", async () => {
    users.mockResolvedValue([{ id: 7, username: 'Pilot' } as never]);
    account.mockResolvedValue({
      ok: true,
      value: {
        account: { id: 7, username: 'Pilot' },
        keys: [ASSIGNED],
        activations: [],
      },
    });
    const wrapper = mount(CdKeysSection);
    await flushPromises();
    await wrapper.get('#cd-account-name').setValue('pilot');
    await wrapper.get('#cd-account-name').element.closest('form')!.dispatchEvent(new Event('submit'));
    await flushPromises();
    expect(account).toHaveBeenCalledWith(7);
    const panel = wrapper.get('#cd-account-panel');
    expect(panel.text()).toContain(ASSIGNED.serial);
    expect(panel.text()).toContain('No machine is activated on this account.');

    users.mockResolvedValue([]);
    await wrapper.get('#cd-account-name').setValue('ghost');
    await wrapper.get('#cd-account-name').element.closest('form')!.dispatchEvent(new Event('submit'));
    await flushPromises();
    expect(wrapper.get('[role="alert"]').text()).toBe('No player account is named ghost.');
  });
});
