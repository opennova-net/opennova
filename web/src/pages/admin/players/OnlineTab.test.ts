import * as actionReview from '../../../composables/useConfirm';
import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi, type MockInstance } from 'vitest';
import OnlineTab from './OnlineTab.vue';
import { endConnection } from '../../../api/admin';
import { fetchOnline } from '../../../api/online';
import { onlinePlayer, onlineResponse } from '../../../test/onlineFixtures';
import { testRouter } from '../../../test/router';

vi.mock('../../../api/online', () => ({ fetchOnline: vi.fn() }));
vi.mock('../../../api/admin', async (importOriginal) => ({ ...(await importOriginal<object>()), endConnection: vi.fn() }));

const online = vi.mocked(fetchOnline);
const end = vi.mocked(endConnection);

// Biggy plays on a hosted server; Rook (account "rookie") waits in the
// lobby; Ghost came through NovaLogic's NovaWorld and has no account here.
const BIGGY = onlinePlayer('Biggy', { account: { id: 7, username: 'biggy', nwhandle: 'Biggy' }, session_id: 41 });
const ROOK = onlinePlayer('Rook', {
  account: { id: 8, username: 'rookie', nwhandle: 'Rook' },
  game: { slug: 'dfx2_consumer', display_name: 'Delta Force Xtreme 2' },
  state: 'lobby',
  server: null,
  session_id: 42,
  client: 'opennova.exe',
});
const GHOST = onlinePlayer('Ghost', {
  account: null,
  via: 'novalogic',
  session_id: null,
  client: null,
  server: { rid: 167772170, name: 'Retail Box', mission: 'Desert Storm', game_type: '', hosted_server_id: null },
});

async function mountTab(path = '/admin/players') {
  const router = await testRouter(path);
  const wrapper = mount(OnlineTab, { global: { plugins: [router] } });
  await flushPromises();
  return { wrapper, router };
}

function names(wrapper: Awaited<ReturnType<typeof mountTab>>['wrapper']) {
  return wrapper.findAll('tr.online-row').map((r) => r.get('.player-link').text());
}

describe('OnlineTab', () => {
  let confirm: MockInstance<typeof actionReview.confirmAction>;

  beforeEach(() => {
    vi.clearAllMocks();
    confirm = vi.spyOn(actionReview, 'confirmAction').mockResolvedValue(true);
    online.mockResolvedValue({ ok: true, value: onlineResponse([BIGGY, ROOK, GHOST]) });
  });

  afterEach(() => confirm.mockRestore());

  it('lists who is online, each linked to their account and the server they play on', async () => {
    const { wrapper } = await mountTab();
    expect(wrapper.get('h1').text()).toBe('Players');
    expect(wrapper.get('.online-count').text()).toBe('3 online · 2 playing');
    expect(wrapper.findAll('thead th').map((th) => th.text())).toEqual(['Player', 'Game', 'Where', 'Since', 'Via', 'Client', 'Actions']);
    const [biggy, rook, ghost] = wrapper.findAll('tr.online-row');

    expect(biggy.get('th a').attributes('href')).toBe('/admin/players/7');
    expect(biggy.get('th').text()).toBe('Biggy');
    expect(biggy.get('.server-link a').attributes('href')).toBe('/admin/servers/1');
    expect(biggy.get('.online-map').text()).toBe('River Patrol · TDM');
    expect(biggy.text()).toContain('Joint Operations');
    expect(biggy.text()).toContain('Jointops.exe');
    expect(biggy.find('.badge').exists()).toBe(false);

    // The account's username beside a name that differs from it.
    expect(rook.get('th a').attributes('href')).toBe('/admin/players/8');
    expect(rook.get('th').text()).toBe('Rook rookie');
    expect(rook.text()).toContain('In the lobby');

    // NovaLogic's player: no account to link, a server of no hosted id links its listing, no session to end.
    expect(ghost.get('th').find('a').exists()).toBe(false);
    expect(ghost.get('th').text()).toBe('Ghost');
    expect(ghost.get('.server-link a').attributes('href')).toBe('/lobby/167772170');
    expect(ghost.get('.badge').text()).toBe('NovaLogic');
    expect(ghost.findAll('button')).toHaveLength(0);
    wrapper.unmount();
  });

  it('filters by name, game and where, in the URL query', async () => {
    const { wrapper, router } = await mountTab();
    await wrapper.get('#online-search').setValue('ROOKIE');
    await flushPromises();
    expect(router.currentRoute.value.query.q).toBe('ROOKIE');
    expect(names(wrapper)).toEqual(['Rook']);
    await wrapper.get('#online-search').setValue('');
    await flushPromises();

    expect(wrapper.findAll('#online-game option').map((o) => o.text())).toEqual(['All games', 'Delta Force Xtreme 2', 'Joint Operations']);
    await wrapper.get('#online-game').setValue('jop_2_consumer');
    await flushPromises();
    expect(names(wrapper)).toEqual(['Biggy', 'Ghost']);

    const chip = (label: string) => wrapper.findAll('[role="group"] button').find((b) => b.text() === label)!;
    await chip('In the lobby').trigger('click');
    await flushPromises();
    expect(router.currentRoute.value.query).toMatchObject({ game: 'jop_2_consumer', state: 'lobby' });
    expect(chip('In the lobby').attributes('aria-pressed')).toBe('true');
    expect(wrapper.get('tbody').text()).toContain('No player matches.');
    wrapper.unmount();

    const again = await mountTab('/admin/players?state=playing');
    expect(names(again.wrapper)).toEqual(['Biggy', 'Ghost']);
    again.wrapper.unmount();
  });

  it('ends a session after a confirmation', async () => {
    end.mockResolvedValue({ ok: true, value: undefined });
    const { wrapper } = await mountTab();
    const button = wrapper.findAll('tr.online-row')[0].get('button');
    expect(button.text()).toContain('End session');

    confirm.mockResolvedValueOnce(false);
    await button.trigger('click');
    await flushPromises();
    expect(end).not.toHaveBeenCalled();

    await button.trigger('click');
    await flushPromises();
    expect(confirm).toHaveBeenLastCalledWith(expect.stringContaining("End Biggy's session?"));
    expect(end).toHaveBeenCalledWith({ session_id: 41 });
    expect(wrapper.get('[role="status"]').text()).toBe("Biggy's session is ending.");
    // And reads again.
    expect(online).toHaveBeenCalledTimes(2);
    wrapper.unmount();
  });

  it('says when nobody is online, and shows a failed read', async () => {
    online.mockResolvedValue({ ok: true, value: onlineResponse([]) });
    const { wrapper } = await mountTab();
    expect(wrapper.get('tbody').text()).toBe('Nobody is online.');
    wrapper.unmount();

    online.mockResolvedValue({ ok: false, error: { code: 'network', status: null, message: 'Could not reach the server.' } });
    const failed = await mountTab();
    expect(failed.wrapper.get('[role="alert"]').text()).toBe('Could not reach the server.');
    expect(failed.wrapper.get('tbody').text()).toBe('Not loaded.');
    failed.wrapper.unmount();
  });
});
