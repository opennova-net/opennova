import { flushPromises, mount, type VueWrapper } from '@vue/test-utils';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import HostedSettingsForm from './HostedSettingsForm.vue';
import { startHostedServer } from '../api/hostedServer';
import { address } from '../test/addressFixtures';
import { DFX2, JO, hostedServer, settings } from '../test/hostedFixtures';
import type { Address } from '../types/addresses';
import type { HostedDataSet, HostedSettings } from '../types/hostedServer';
import { MISSIONS_MAX } from '../utils/hostedSettings';

vi.mock('../api/hostedServer', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../api/hostedServer')>()),
  startHostedServer: vi.fn(),
}));
const start = vi.mocked(startHostedServer);

const ESCALATION = JO.expansions[0].id;

function mountForm(
  dataSets: HostedDataSet[] = [JO, DFX2],
  previous: HostedSettings | null = null,
  extra: {
    baseId?: string;
    sharedFree?: number;
    ownMachinesFree?: number;
    previousOwnMachine?: boolean;
    addresses?: Address[];
    preferredAddressId?: number | null;
  } = {},
) {
  return mount(HostedSettingsForm, { props: { previous, dataSets, ...extra } });
}

function radios(wrapper: VueWrapper, name: string) {
  return wrapper.findAll(`input[type="radio"][name="hosted-settings-${name}"]`);
}

function radio(wrapper: VueWrapper, name: string, value: string) {
  return wrapper.get(`input[type="radio"][name="hosted-settings-${name}"][value="${value}"]`);
}

// The rotation as the picker lists it, in order: each row's first line (the
// title, or the file when the data names none).
function rotation(wrapper: VueWrapper): string[] {
  return wrapper.findAll('ol li span.flex-1 > span:first-child').map((s) => s.text());
}

describe('HostedSettingsForm', () => {
  beforeEach(() => {
    start.mockReset();
  });

  it('offers each game first with its version, and the picked game drives the rest of the form', async () => {
    const wrapper = mountForm();
    const games = radios(wrapper, 'data_set_id');
    expect(games.map((g) => (g.element as HTMLInputElement).value)).toEqual([String(JO.id), String(DFX2.id)]);
    expect(wrapper.text()).toContain('Delta Force Xtreme 2');
    expect(wrapper.text()).toContain('1.7.5.5');
    // Before any other field.
    const first = wrapper.get('form').element.querySelector('input');
    expect(first?.getAttribute('name')).toBe('hosted-settings-data_set_id');
    expect((radio(wrapper, 'data_set_id', String(JO.id)).element as HTMLInputElement).checked).toBe(true);

    // JO: the base game or its expansion, its ranges, every mission of its data
    // by title (the file when the data names none).
    expect(radios(wrapper, 'expansion_id')).toHaveLength(2);
    expect(wrapper.get('#hosted-settings-max_players').attributes('max')).toBe('64');
    expect(wrapper.get('#hosted-settings-max_players-hint').text()).toBe('2 to 64.');
    expect(wrapper.get('#hosted-settings-kill_limit-hint').text()).toBe('1 to 500. 500 means no limit.');
    expect(rotation(wrapper)).toEqual(['River Patrol', 'jo_two.bms']);

    // Its expansion: its missions join the rotation.
    await radio(wrapper, 'expansion_id', String(ESCALATION)).setValue();
    expect(rotation(wrapper)).toEqual(['River Patrol', 'jo_two.bms', 'Escalation Point']);

    // DFX2: no expansion to choose (the form's goes back to the base game), its
    // ranges, its rotation.
    await radio(wrapper, 'data_set_id', String(DFX2.id)).setValue();
    expect(radios(wrapper, 'expansion_id')).toHaveLength(0);
    expect(wrapper.get('#hosted-settings-max_players').attributes('max')).toBe('50');
    expect(wrapper.get('#hosted-settings-respawn').attributes('max')).toBe('60');
    expect(wrapper.get('#hosted-settings-kill_limit-hint').text()).toBe('1 to 250. 250 means no limit.');
    expect(rotation(wrapper)).toEqual(['Dust']);

    // Its limits apply to the check: 60 players is too many for DFX2.
    await wrapper.get('#hosted-settings-game_name').setValue('Night Ops');
    await wrapper.get('#hosted-settings-max_players').setValue(60);
    await wrapper.get('form').trigger('submit');
    await flushPromises();
    expect(start).not.toHaveBeenCalled();
    expect(wrapper.get('#hosted-settings-max_players-error').text()).toBe('Enter a whole number from 2 to 50.');

    // In range, the start sends the ids with the rest, on a shared machine.
    start.mockResolvedValue({ ok: true, value: hostedServer(9, { state: 'requested' }) });
    await wrapper.get('#hosted-settings-max_players').setValue(24);
    await wrapper.get('form').trigger('submit');
    await flushPromises();
    expect(start).toHaveBeenCalledTimes(1);
    expect(start.mock.calls[0][0]).toMatchObject({
      data_set_id: DFX2.id,
      expansion_id: null,
      game_name: 'Night Ops',
      max_players: 24,
      missions: ['dfx_one.bms'],
    });
    expect(start.mock.calls[0][0]).not.toHaveProperty('game');
    expect(start.mock.calls[0][1]).toBe(false);
    expect(wrapper.emitted('started')?.[0]).toEqual([hostedServer(9, { state: 'requested' })]);
  });

  it('sends the expansion by id with every mission of base and expansion', async () => {
    start.mockResolvedValue({ ok: true, value: hostedServer(9, { state: 'requested' }) });
    const wrapper = mountForm();
    await radio(wrapper, 'expansion_id', String(ESCALATION)).setValue();
    await wrapper.get('#hosted-settings-game_name').setValue('Escalation Ops');
    await wrapper.get('form').trigger('submit');
    await flushPromises();
    expect(start.mock.calls[0][0]).toMatchObject({
      data_set_id: JO.id,
      expansion_id: ESCALATION,
      missions: ['jo_one.bms', 'jo_two.bms', 'jox_one.bms'],
    });
  });

  it('lets the rotation hold every mission up to the cap, and no more', async () => {
    const many = Array.from({ length: MISSIONS_MAX + 1 }, (_, i) => ({ file: `m${i}.bms`, title: `Mission ${i}` }));
    const big: HostedDataSet = { ...JO, missions: many, expansions: [] };
    const wrapper = mountForm([big]);
    // Every mission by default: the cap holds, and the form says so.
    expect(rotation(wrapper)).toHaveLength(MISSIONS_MAX + 1);
    await wrapper.get('#hosted-settings-game_name').setValue('Big');
    await wrapper.get('form').trigger('submit');
    await flushPromises();
    expect(start).not.toHaveBeenCalled();
    expect(wrapper.get('#hosted-settings-missions-error').text()).toBe(
      `A rotation holds ${MISSIONS_MAX} missions at most; this one has ${MISSIONS_MAX + 1}.`,
    );
    // One removed: it fits.
    start.mockResolvedValue({ ok: true, value: hostedServer(9, { state: 'requested' }) });
    await wrapper.get('#hosted-settings-missions-remove-m0\\.bms').trigger('click');
    await wrapper.get('form').trigger('submit');
    await flushPromises();
    expect(start).toHaveBeenCalledTimes(1);
    expect(start.mock.calls[0][0].missions).toHaveLength(MISSIONS_MAX);
  });

  it('takes its ids from baseId, and offers a dedicated machine only while one is free', async () => {
    const shared = mountForm([JO], null, { baseId: 'new-server' });
    expect(shared.find('#new-server-game_name').exists()).toBe(true);
    expect(shared.find('#hosted-settings-game_name').exists()).toBe(false);
    expect(shared.find('#new-server-own_machine').exists()).toBe(false);

    start.mockResolvedValue({ ok: true, value: hostedServer(4, { state: 'requested', own_machine: true }) });
    const own = mountForm([JO], null, { baseId: 'new-server', ownMachinesFree: 1 });
    expect(own.get('#new-server-own_machine-hint').text()).toContain('One of your dedicated machines is free.');
    // A choice of where it runs, a shared machine first.
    expect(own.get('#new-server-pool legend').text()).toBe('Where it runs');
    expect((own.get('#new-server-shared_machine').element as HTMLInputElement).checked).toBe(true);
    await own.get('#new-server-game_name').setValue('Night Ops 2');
    await own.get('#new-server-own_machine').setValue(true);
    await own.get('form').trigger('submit');
    await flushPromises();
    expect(start).toHaveBeenCalledWith(expect.objectContaining({ game_name: 'Night Ops 2' }), true, null);
  });

  it('offers the free static addresses on a dedicated machine, the preferred one first', async () => {
    start.mockResolvedValue({ ok: true, value: hostedServer(4, { state: 'requested', own_machine: true }) });
    const free = [address(1, 'smoke'), address(2, 'arena')];
    const wrapper = mountForm([JO], null, { ownMachinesFree: 1, addresses: free, preferredAddressId: 2 });
    // Only once the dedicated machine is picked.
    expect(wrapper.find('#hosted-settings-address_id').exists()).toBe(false);
    await wrapper.get('#hosted-settings-own_machine').setValue(true);
    const select = wrapper.get('#hosted-settings-address_id');
    expect(select.findAll('option').map((o) => o.text())).toEqual([
      'No static address',
      'smoke.play.opennova.net',
      'arena.play.opennova.net',
    ]);
    expect((select.element as HTMLSelectElement).selectedIndex).toBe(2);
    await wrapper.get('#hosted-settings-game_name').setValue('Night Ops');
    await wrapper.get('form').trigger('submit');
    await flushPromises();
    expect(start).toHaveBeenLastCalledWith(expect.objectContaining({ game_name: 'Night Ops' }), true, 2);

    // None picked: no address goes.
    (select.element as HTMLSelectElement).selectedIndex = 0;
    await select.trigger('change');
    await wrapper.get('form').trigger('submit');
    await flushPromises();
    expect(start).toHaveBeenLastCalledWith(expect.anything(), true, null);

    // A shared machine never names one.
    await wrapper.get('#hosted-settings-shared_machine').setValue();
    await wrapper.get('form').trigger('submit');
    await flushPromises();
    expect(start).toHaveBeenLastCalledWith(expect.anything(), false, null);
  });

  it('starts the pick on the first free address when the preferred one is taken, and hides it with none free', async () => {
    const first = mountForm([JO], null, { ownMachinesFree: 1, addresses: [address(1, 'smoke')], preferredAddressId: 9 });
    await first.get('#hosted-settings-own_machine').setValue(true);
    expect((first.get('#hosted-settings-address_id').element as HTMLSelectElement).selectedIndex).toBe(1);

    const none = mountForm([JO], null, { ownMachinesFree: 1, addresses: [] });
    await none.get('#hosted-settings-own_machine').setValue(true);
    expect(none.find('#hosted-settings-address_id').exists()).toBe(false);
  });

  it('starts on a dedicated machine when the server started again ran on one, while one is free', async () => {
    const again = mountForm([JO], settings(), {
      ownMachinesFree: 1,
      previousOwnMachine: true,
      addresses: [address(1, 'smoke'), address(2, 'arena')],
      preferredAddressId: 2,
    });
    expect((again.get('#hosted-settings-own_machine').element as HTMLInputElement).checked).toBe(true);
    expect((again.get('#hosted-settings-address_id').element as HTMLSelectElement).selectedIndex).toBe(2);

    const none = mountForm([JO], settings(), { ownMachinesFree: 0, previousOwnMachine: true });
    expect(none.find('#hosted-settings-own_machine').exists()).toBe(false);
  });

  it('runs on a dedicated machine with no server on shared machines free', async () => {
    start.mockResolvedValue({ ok: true, value: hostedServer(4, { state: 'requested', own_machine: true }) });
    const wrapper = mountForm([JO], null, { sharedFree: 0, ownMachinesFree: 1, addresses: [address(1, 'smoke')] });
    expect(wrapper.find('#hosted-settings-own_machine').exists()).toBe(false);
    expect(wrapper.get('#hosted-settings-pool').text()).toBe('On your dedicated machine');
    // Its static addresses at once.
    expect((wrapper.get('#hosted-settings-address_id').element as HTMLSelectElement).selectedIndex).toBe(1);
    await wrapper.get('#hosted-settings-game_name').setValue('Night Ops');
    await wrapper.get('form').trigger('submit');
    await flushPromises();
    expect(start).toHaveBeenLastCalledWith(expect.objectContaining({ game_name: 'Night Ops' }), true, 1);
  });

  it('marks the game picker when the backend refuses the data set', async () => {
    start.mockResolvedValue({
      ok: false,
      error: {
        code: 'invalid_settings',
        status: 400,
        message: 'The server refused the settings. Pick a game this service hosts.',
        field: 'data_set_id',
      },
    });
    const wrapper = mountForm();
    await wrapper.get('#hosted-settings-game_name').setValue('Night Ops');
    await wrapper.get('form').trigger('submit');
    await flushPromises();
    expect(start).toHaveBeenCalledTimes(1);
    expect(start.mock.calls[0][0]).toMatchObject({ data_set_id: JO.id, expansion_id: null });
    const error = wrapper.get('#hosted-settings-data_set_id-error');
    expect(error.text()).toBe('Pick a game this service hosts.');
    const picker = wrapper.get('#hosted-settings-data_set_id').element.closest('fieldset');
    expect(picker?.getAttribute('aria-describedby')).toContain('hosted-settings-data_set_id-error');
    expect(wrapper.emitted('refused')).toHaveLength(1);
    // Another game takes the mark away.
    await radio(wrapper, 'data_set_id', String(DFX2.id)).setValue();
    expect(wrapper.find('#hosted-settings-data_set_id-error').exists()).toBe(false);
  });

  it("starts again from the last server's game, expansion and rotation", async () => {
    const last = settings({ data_set_id: DFX2.id, max_players: 40, missions: ['dfx_one.bms', 'gone.bms'] });
    const wrapper = mountForm([JO, DFX2], last);
    expect((radio(wrapper, 'data_set_id', String(DFX2.id)).element as HTMLInputElement).checked).toBe(true);
    expect(rotation(wrapper)).toEqual(['Dust']);
    expect(wrapper.text()).toContain('gone.bms');
    expect((wrapper.get('#hosted-settings-max_players').element as HTMLInputElement).value).toBe('40');
    // Another game: every mission of its data, and nothing said left out.
    await radio(wrapper, 'data_set_id', String(JO.id)).setValue();
    expect(rotation(wrapper)).toEqual(['River Patrol', 'jo_two.bms']);
    expect(wrapper.text()).not.toContain('gone.bms');

    // The last server's expansion is picked again, with its rotation.
    const withExpansion = mountForm([JO, DFX2], settings({ expansion_id: ESCALATION, missions: ['jox_one.bms'] }));
    expect((radio(withExpansion, 'expansion_id', String(ESCALATION)).element as HTMLInputElement).checked).toBe(true);
    expect(rotation(withExpansion)).toEqual(['Escalation Point']);
  });

  it("says so when the last server's game is no longer offered", () => {
    const wrapper = mountForm([JO], settings({ data_set_id: DFX2.id }));
    expect((radio(wrapper, 'data_set_id', String(JO.id)).element as HTMLInputElement).checked).toBe(true);
    expect(wrapper.text()).toContain('Your last server ran a game this service no longer hosts');
  });
});
