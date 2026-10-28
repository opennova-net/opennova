import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import type { Router } from 'vue-router';
import HostedGamePage from './HostedGamePage.vue';
import { fetchListingGames, listMyServers, sendServerCommand, stopHosting } from '../../api/myServers';
import { confirmAction } from '../../composables/useConfirm';
import { listedServer } from '../../test/hostedFixtures';
import { testRouter } from '../../test/router';
import type { MyServer, RosterSlot } from '../../types/myServers';

vi.mock('../../api/myServers', () => ({
  listMyServers: vi.fn(),
  sendServerCommand: vi.fn(),
  stopHosting: vi.fn(),
  fetchListingGames: vi.fn(),
}));
vi.mock('../../composables/useConfirm', () => ({ confirmAction: vi.fn() }));

const list = vi.mocked(listMyServers);
const send = vi.mocked(sendServerCommand);
const ranger: RosterSlot = { slot: 1, player_name: 'Ranger', team: '1', type: '0' };

let page: ReturnType<typeof mount> | null = null;
let router: Router;

async function render(rows: MyServer[] = [listedServer(30, { roster: [ranger], msg: 'Welcome' })]) {
  list.mockResolvedValue({ ok: true, value: rows });
  router = await testRouter('/me/servers/hosts/30', ['/me/servers/hosts/:rid']);
  page = mount(HostedGamePage, { global: { plugins: [router] }, attachTo: document.body });
  await flushPromises();
  return page;
}

const button = (text: string) => page!.findAll('button').find((b) => b.text() === text);

describe('HostedGamePage', () => {
  beforeEach(() => {
    vi.clearAllMocks();
    vi.mocked(fetchListingGames).mockResolvedValue(new Map([[30, 'Joint Operations']]));
    vi.mocked(confirmAction).mockResolvedValue(true);
    send.mockResolvedValue({ ok: true, value: { rid: 30, status: 'queued', statement: 'ServerCommand' } });
  });

  afterEach(() => {
    page?.unmount();
    page = null;
  });

  it('heads the page with the server, its game, map, players, address and public page', async () => {
    await render();
    expect(page!.get('h1').text()).toBe('Listed 30');
    expect(page!.get('.page-crumbs a').attributes('href')).toBe('/me/servers');
    const meta = page!.get('.page-meta');
    expect(meta.get('.server-game').text()).toBe('Joint Operations');
    expect(meta.get('.server-map').text()).toBe('jo_one.bms');
    expect(meta.get('.server-game-type').text()).toBe('TDM');
    expect(meta.get('.server-players').text()).toBe('2/32 players');
    expect(meta.get('.server-address').text()).toBe('203.0.113.7:32769');
    expect(meta.findAll('a').find((a) => a.text() === 'Public page')?.attributes('href')).toBe('/lobby/30');
  });

  it('lists its players in a table under a heading, and each control under one', async () => {
    await render();
    expect(page!.findAll('h2').map((h) => h.text())).toEqual(['Players', 'Chat', 'Server', 'Mission', 'Weather and time']);
    expect(page!.findAll('thead th').map((th) => th.text())).toEqual(['Slot', 'Player', 'Team', 'Actions']);
    expect(page!.get('tbody th').text()).toBe('Ranger');
    // The fields follow the server's own name and message.
    expect((page!.get('#game-server-name').element as HTMLInputElement).value).toBe('Listed 30');
    expect((page!.get('#game-server-msg').element as HTMLInputElement).value).toBe('Welcome');
  });

  it('sends the chat, the name, the message, the weather and the time', async () => {
    await render();
    await page!.get('#game-chat-text').setValue('gg all');
    await page!.get('#game-chat-text').element.closest('form')!.dispatchEvent(new Event('submit'));
    await flushPromises();
    expect(send).toHaveBeenLastCalledWith(30, { verb: 'TextChatServer', args: ['gg all'] });
    expect(page!.get('[role="status"]').text()).toBe('Sent: “gg all”.');

    await page!.get('#game-server-name').setValue('Night Raid');
    await page!.get('#game-server-name').element.closest('form')!.dispatchEvent(new Event('submit'));
    await flushPromises();
    expect(send).toHaveBeenLastCalledWith(30, { verb: 'SetServerName', args: ['Night Raid'] });

    await page!.get('#game-server-msg').setValue('Be nice');
    await page!.get('#game-server-msg').element.closest('form')!.dispatchEvent(new Event('submit'));
    await flushPromises();
    expect(send).toHaveBeenLastCalledWith(30, { verb: 'SetServerMsg', args: ['Be nice'] });

    await button('Lightning')!.trigger('click');
    await flushPromises();
    expect(send).toHaveBeenLastCalledWith(30, { verb: 'Lightning' });

    await page!.get('#game-quake').setValue('12');
    await page!.get('#game-quake').element.closest('form')!.dispatchEvent(new Event('submit'));
    await flushPromises();
    expect(send).toHaveBeenLastCalledWith(30, { verb: 'Earthquake', args: ['12'] });

    await page!.get('#game-time-of-day').setValue('1330');
    await page!.get('#game-time-of-day').element.closest('form')!.dispatchEvent(new Event('submit'));
    await flushPromises();
    expect(send).toHaveBeenLastCalledWith(30, { verb: 'TimeOfDay', args: ['1330'] });
  });

  it('ends the mission with a winner after a one-sentence confirmation', async () => {
    await render();
    await page!.get('#game-winner').setValue('Red');
    await button('End mission')!.trigger('click');
    await flushPromises();
    expect(confirmAction).toHaveBeenCalledWith('End the mission on Listed 30? The mission ends for everyone; Red wins.');
    expect(send).toHaveBeenCalledWith(30, { verb: 'EndMission', args: ['Red'] });
  });

  it('kicks a player after a confirmation, aimed at their slot', async () => {
    await render();
    await page!.findAll('button').find((b) => b.text().startsWith('Kick'))!.trigger('click');
    await flushPromises();
    expect(confirmAction).toHaveBeenCalledWith('Kick Ranger from Listed 30?');
    expect(send).toHaveBeenCalledWith(30, { verb: 'PuntPlayer', target: 'ByIndex', args: ['1'] });
  });

  it.each([
    ['Kick', [{ ...ranger, player_name: 'Replacement' }]],
    ['Kill', []],
    ['Switch team', [{ ...ranger, player_name: 'Replacement' }]],
  ] as const)('cancels %s when the confirmed player no longer holds the slot', async (label, roster) => {
    let approve: (accepted: boolean) => void = () => {};
    vi.mocked(confirmAction).mockImplementation(() => new Promise((resolve) => (approve = resolve)));
    await render();
    await page!.findAll('button').find((b) => b.text().startsWith(label))!.trigger('click');
    // The roster moves on while the question is open.
    list.mockResolvedValue({ ok: true, value: [listedServer(30, { roster: [...roster] })] });
    await button('Refresh')!.trigger('click');
    await flushPromises();
    approve(true);
    await flushPromises();
    expect(send).not.toHaveBeenCalled();
    expect(page!.get('[role="alert"]').text()).toContain('The player list changed');
  });

  it('stops hosting after a one-sentence confirmation, and says it is gone', async () => {
    vi.mocked(stopHosting).mockResolvedValue({ ok: true, value: { rid: 30, status: 'queued', statement: 'ServerStopHosting' } });
    await render();
    await button('Stop hosting…')!.trigger('click');
    await flushPromises();
    expect(confirmAction).toHaveBeenCalledWith('Stop hosting Listed 30? Its match ends for everyone.');
    expect(stopHosting).toHaveBeenCalledWith(30);
    expect(page!.get('[role="status"]').text()).toBe('Stopped hosting Listed 30.');
    expect(page!.get('.server-gone').text()).toContain('This server is no longer listed.');
    expect(page!.find('table').exists()).toBe(false);
  });

  it('keeps hosting when the stop is not confirmed', async () => {
    vi.mocked(confirmAction).mockResolvedValue(false);
    await render();
    await button('Stop hosting…')!.trigger('click');
    await flushPromises();
    expect(stopHosting).not.toHaveBeenCalled();
    expect(page!.find('.server-gone').exists()).toBe(false);
  });

  it('says so when the listing is gone, with the way back', async () => {
    await render([listedServer(31)]);
    const gone = page!.get('.server-gone');
    expect(gone.text()).toContain('This server is no longer listed.');
    expect(gone.get('a').attributes('href')).toBe('/me/servers');
    expect(page!.find('h2').exists()).toBe(false);
  });

  it('reads the list again when a push finds the listing gone', async () => {
    await render();
    send.mockResolvedValue({ ok: false, error: { code: 'unknown_rid', status: 404, message: 'This server is no longer listed under your account. Refresh the list.' } });
    list.mockResolvedValue({ ok: true, value: [] });
    await button('Lightning')!.trigger('click');
    await flushPromises();
    expect(page!.find('.server-gone').exists()).toBe(true);
  });

  it("opens a hosted server's page for its listing", async () => {
    await render([listedServer(30, { hosted_server_id: 4 })]);
    expect(router.currentRoute.value.fullPath).toBe('/me/servers/4');
  });
});
