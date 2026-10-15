import { flushPromises, mount, type VueWrapper } from '@vue/test-utils';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import HostedSettingsForm from './HostedSettingsForm.vue';
import { startHostedServer } from '../api/hostedServer';
import { DFX2, JO, hostedServer, settings } from '../test/hostedFixtures';
import type { HostedDataSet, HostedSettings } from '../types/hostedServer';

vi.mock('../api/hostedServer', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../api/hostedServer')>()),
  startHostedServer: vi.fn(),
}));
const start = vi.mocked(startHostedServer);

function mountForm(
  dataSets: HostedDataSet[] = [JO, DFX2],
  previous: HostedSettings | null = null,
  extra: { baseId?: string; ownMachinesFree?: number } = {},
) {
  return mount(HostedSettingsForm, { props: { previous, dataSets, ...extra } });
}

function radios(wrapper: VueWrapper, name: string) {
  return wrapper.findAll(`input[type="radio"][name="hosted-settings-${name}"]`);
}

function radio(wrapper: VueWrapper, name: string, value: string) {
  return wrapper.get(`input[type="radio"][name="hosted-settings-${name}"][value="${value}"]`);
}

// The rotation as the picker lists it, in order.
function rotation(wrapper: VueWrapper): string[] {
  return wrapper.findAll('ol li span.break-all').map((s) => s.text());
}

describe('HostedSettingsForm', () => {
  beforeEach(() => {
    start.mockReset();
  });

  it('offers each game first, and the picked game drives the rest of the form', async () => {
    const wrapper = mountForm();
    const games = radios(wrapper, 'game');
    expect(games.map((g) => (g.element as HTMLInputElement).value)).toEqual([JO.game, DFX2.game]);
    expect(wrapper.text()).toContain('Delta Force Xtreme 2');
    // Before any other field.
    const first = wrapper.get('form').element.querySelector('input');
    expect(first?.getAttribute('name')).toBe('hosted-settings-game');
    expect((radio(wrapper, 'game', JO.game).element as HTMLInputElement).checked).toBe(true);

    // JO: its two expansions, its ranges, its rotation.
    expect(radios(wrapper, 'expansion')).toHaveLength(2);
    expect(wrapper.get('#hosted-settings-max_players').attributes('max')).toBe('64');
    expect(wrapper.get('#hosted-settings-max_players-hint').text()).toBe('2 to 64.');
    expect(wrapper.get('#hosted-settings-kill_limit-hint').text()).toBe('1 to 500. 500 means no limit.');
    expect(rotation(wrapper)).toEqual(['jo_one.bms', 'jo_two.bms']);
    await radio(wrapper, 'expansion', 'jox01').setValue();

    // DFX2: no expansion to choose (the form's goes back to the base game), its
    // ranges, its rotation.
    await radio(wrapper, 'game', DFX2.game).setValue();
    expect(radios(wrapper, 'expansion')).toHaveLength(0);
    expect(wrapper.get('#hosted-settings-max_players').attributes('max')).toBe('50');
    expect(wrapper.get('#hosted-settings-respawn').attributes('max')).toBe('60');
    expect(wrapper.get('#hosted-settings-kill_limit-hint').text()).toBe('1 to 250. 250 means no limit.');
    expect(rotation(wrapper)).toEqual(['dfx_one.bms']);

    // Its limits apply to the check: 60 players is too many for DFX2.
    await wrapper.get('#hosted-settings-game_name').setValue('Night Ops');
    await wrapper.get('#hosted-settings-max_players').setValue(60);
    await wrapper.get('form').trigger('submit');
    await flushPromises();
    expect(start).not.toHaveBeenCalled();
    expect(wrapper.get('#hosted-settings-max_players-error').text()).toBe('Enter a whole number from 2 to 50.');

    // In range, the start sends the game with the rest, on a shared machine.
    start.mockResolvedValue({ ok: true, value: hostedServer(9, { state: 'requested' }) });
    await wrapper.get('#hosted-settings-max_players').setValue(24);
    await wrapper.get('form').trigger('submit');
    await flushPromises();
    expect(start).toHaveBeenCalledTimes(1);
    expect(start.mock.calls[0][0]).toMatchObject({
      game: DFX2.game,
      game_name: 'Night Ops',
      max_players: 24,
      expansion: '',
      missions: ['dfx_one.bms'],
    });
    expect(start.mock.calls[0][1]).toBe(false);
    expect(wrapper.emitted('started')?.[0]).toEqual([hostedServer(9, { state: 'requested' })]);
  });

  it('takes its ids from baseId, and offers an own machine only while one is free', async () => {
    const shared = mountForm([JO], null, { baseId: 'new-server' });
    expect(shared.find('#new-server-game_name').exists()).toBe(true);
    expect(shared.find('#hosted-settings-game_name').exists()).toBe(false);
    expect(shared.find('#new-server-own_machine').exists()).toBe(false);

    start.mockResolvedValue({ ok: true, value: hostedServer(4, { state: 'requested', own_machine: true }) });
    const own = mountForm([JO], null, { baseId: 'new-server', ownMachinesFree: 1 });
    expect(own.get('#new-server-own_machine-hint').text()).toContain('One of your own machines is free.');
    await own.get('#new-server-game_name').setValue('Night Ops 2');
    await own.get('#new-server-own_machine').setValue(true);
    await own.get('form').trigger('submit');
    await flushPromises();
    expect(start).toHaveBeenCalledWith(expect.objectContaining({ game_name: 'Night Ops 2' }), true);
  });

  it("marks the game picker when the backend refuses the game", async () => {
    start.mockResolvedValue({
      ok: false,
      error: {
        code: 'invalid_settings',
        status: 400,
        message: 'The server refused the settings. Pick a game this service hosts.',
        field: 'game',
      },
    });
    const wrapper = mountForm();
    await wrapper.get('#hosted-settings-game_name').setValue('Night Ops');
    await wrapper.get('form').trigger('submit');
    await flushPromises();
    expect(start).toHaveBeenCalledTimes(1);
    expect(start.mock.calls[0][0]).toMatchObject({ game: JO.game, expansion: '' });
    const error = wrapper.get('#hosted-settings-game-error');
    expect(error.text()).toBe('Pick a game this service hosts.');
    const picker = wrapper.get('#hosted-settings-game').element.closest('fieldset');
    expect(picker?.getAttribute('aria-describedby')).toContain('hosted-settings-game-error');
    expect(wrapper.emitted('refused')).toHaveLength(1);
    // Another game takes the mark away.
    await radio(wrapper, 'game', DFX2.game).setValue();
    expect(wrapper.find('#hosted-settings-game-error').exists()).toBe(false);
  });

  it("starts again from the last server's game and rotation", async () => {
    const last = settings({ game: DFX2.game, max_players: 40, missions: ['dfx_one.bms', 'gone.bms'] });
    const wrapper = mountForm([JO, DFX2], last);
    expect((radio(wrapper, 'game', DFX2.game).element as HTMLInputElement).checked).toBe(true);
    expect(rotation(wrapper)).toEqual(['dfx_one.bms']);
    expect(wrapper.text()).toContain('gone.bms');
    expect((wrapper.get('#hosted-settings-max_players').element as HTMLInputElement).value).toBe('40');
    // Another game: its own rotation, and nothing said left out.
    await radio(wrapper, 'game', JO.game).setValue();
    expect(rotation(wrapper)).toEqual(['jo_one.bms', 'jo_two.bms']);
    expect(wrapper.text()).not.toContain('gone.bms');
  });

  it("says so when the last server's game is no longer offered", () => {
    const wrapper = mountForm([JO], settings({ game: DFX2.game }));
    expect((radio(wrapper, 'game', JO.game).element as HTMLInputElement).checked).toBe(true);
    expect(wrapper.text()).toContain('Your last server ran a game this service no longer hosts');
  });
});
