import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import MyServerCard from './MyServerCard.vue';
import { sendServerCommand } from '../api/myServers';
import { confirmAction } from '../composables/useConfirm';
import { listedServer } from '../test/hostedFixtures';
import type { RosterSlot } from '../types/myServers';

vi.mock('../api/myServers', () => ({ sendServerCommand: vi.fn(), stopHosting: vi.fn() }));
vi.mock('../composables/useConfirm', () => ({ confirmAction: vi.fn() }));

const original: RosterSlot = { slot: 1, player_name: 'Ranger', team: '1', type: '0' };

describe('MyServerCard player confirmation', () => {
  let page: ReturnType<typeof mount> | null = null;
  let approve: (accepted: boolean) => void;

  beforeEach(() => {
    vi.clearAllMocks();
    vi.mocked(confirmAction).mockImplementation(() => new Promise((resolve) => { approve = resolve; }));
    vi.mocked(sendServerCommand).mockResolvedValue({ ok: true, value: { rid: 10, status: 'queued', statement: 'ServerCommand' } });
  });
  afterEach(() => { page?.unmount(); });

  async function begin(label: string) {
    page = mount(MyServerCard, { props: { server: listedServer(10, { roster: [original] }), now: Date.now() } });
    await page.findAll('button').find((button) => button.text().startsWith(label))!.trigger('click');
    expect(sendServerCommand).not.toHaveBeenCalled();
  }

  it.each([
    ['Kick', [{ ...original, player_name: 'Replacement' }]],
    ['Kill', []],
    ['Switch team', [{ ...original, player_name: 'Replacement' }]],
  ] as const)('cancels %s when the confirmed player no longer occupies the slot', async (label, roster) => {
    await begin(label);
    await page!.setProps({ server: listedServer(10, { roster: [...roster] }) });
    approve(true);
    await flushPromises();
    expect(sendServerCommand).not.toHaveBeenCalled();
    expect(page!.get('[role="alert"]').text()).toContain('The player list changed');
  });

  it('sends the confirmed command when the player remains in the slot', async () => {
    await begin('Kick');
    await page!.setProps({ server: listedServer(10, { roster: [{ ...original, team: '2' }] }) });
    approve(true);
    await flushPromises();
    expect(sendServerCommand).toHaveBeenCalledWith(10, { verb: 'PuntPlayer', target: 'ByIndex', args: ['1'] });
  });
});
