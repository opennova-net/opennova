import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import MyServerCard from './MyServerCard.vue';
import { sendServerCommand, stopHosting } from '../api/myServers';
import { confirmAction } from '../composables/useConfirm';
import { listedServer } from '../test/hostedFixtures';
import { testRouter } from '../test/router';
import type { MyServer, RosterSlot } from '../types/myServers';

vi.mock('../api/myServers', () => ({ sendServerCommand: vi.fn(), stopHosting: vi.fn() }));
vi.mock('../composables/useConfirm', () => ({ confirmAction: vi.fn() }));

const original: RosterSlot = { slot: 1, player_name: 'Ranger', team: '1', type: '0' };

let page: ReturnType<typeof mount> | null = null;
afterEach(() => {
  page?.unmount();
  page = null;
});

async function render(props: { server: MyServer; embedded?: boolean }) {
  const router = await testRouter('/me/servers');
  page = mount(MyServerCard, { props: { now: Date.now(), ...props }, global: { plugins: [router] } });
  return page;
}

describe('MyServerCard player confirmation', () => {
  let approve: (accepted: boolean) => void;

  beforeEach(() => {
    vi.clearAllMocks();
    vi.mocked(confirmAction).mockImplementation(() => new Promise((resolve) => { approve = resolve; }));
    vi.mocked(sendServerCommand).mockResolvedValue({ ok: true, value: { rid: 10, status: 'queued', statement: 'ServerCommand' } });
  });

  async function begin(label: string) {
    await render({ server: listedServer(10, { roster: [original] }) });
    await page!.findAll('button').find((button) => button.text().startsWith(label))!.trigger('click');
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

describe('MyServerCard', () => {
  beforeEach(() => {
    vi.clearAllMocks();
  });

  it('names the server to its public page and lists its players in a table', async () => {
    const card = await render({ server: listedServer(10, { roster: [original] }) });
    expect(card.get('h3 a').attributes('href')).toBe('/lobby/10');
    expect(card.findAll('thead th').map((th) => th.text())).toEqual(['Slot', 'Player', 'Team', 'Actions']);
    expect(card.get('tbody th').text()).toBe('Ranger');
  });

  it('stops hosting after a one-sentence confirmation', async () => {
    vi.mocked(confirmAction).mockResolvedValue(true);
    vi.mocked(stopHosting).mockResolvedValue({ ok: true, value: { rid: 10, status: 'queued', statement: 'ServerStopHosting' } });
    const card = await render({ server: listedServer(10) });
    await card.findAll('button').find((b) => b.text() === 'Stop hosting')!.trigger('click');
    await flushPromises();
    expect(confirmAction).toHaveBeenCalledWith('Stop hosting Listed 10? Its match ends for everyone.');
    expect(stopHosting).toHaveBeenCalledWith(10);
    expect(card.emitted('stopped')).toEqual([[10, 'Listed 10']]);
  });

  it('keeps hosting when the stop is not confirmed', async () => {
    vi.mocked(confirmAction).mockResolvedValue(false);
    const card = await render({ server: listedServer(10) });
    await card.findAll('button').find((b) => b.text() === 'Stop hosting')!.trigger('click');
    await flushPromises();
    expect(stopHosting).not.toHaveBeenCalled();
  });

  it('shows only the controls a console lacks when embedded', async () => {
    const card = await render({ server: listedServer(10, { roster: [original] }), embedded: true });
    expect(card.find('h3').exists()).toBe(false);
    expect(card.find('table').exists()).toBe(false);
    expect(card.findAll('button').some((b) => b.text() === 'Stop hosting')).toBe(false);
    expect(card.findAll('button').some((b) => b.text() === 'Rename')).toBe(true);
  });
});
