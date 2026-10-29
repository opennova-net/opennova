import { flushPromises, mount, type VueWrapper } from '@vue/test-utils';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import HostedSettingsForm from './HostedSettingsForm.vue';
import { startHostedServer } from '../api/hostedServer';
import { address } from '../test/addressFixtures';
import { DFX2, JO, hostedServer, settings } from '../test/hostedFixtures';
import type { Address } from '../types/addresses';
import type { HostedProduct, HostedSettings } from '../types/hostedServer';
import { FIELD_RULES, MISSIONS_MAX } from '../utils/hostedSettings';
import { useUnsavedChanges } from '../composables/useUnsavedChanges';

vi.mock('../composables/useUnsavedChanges', () => ({ useUnsavedChanges: vi.fn() }));

vi.mock('../api/hostedServer', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../api/hostedServer')>()),
  startHostedServer: vi.fn(),
}));
const start = vi.mocked(startHostedServer);

const ESCALATION = JO.expansions[0].id;

function mountForm(
  products: HostedProduct[] = [JO, DFX2],
  previous: HostedSettings | null = null,
  extra: {
    baseId?: string;
    sharedFree?: number;
    ownMachinesFree?: number;
    previousOwnMachine?: boolean;
    addresses?: Address[];
    preferredAddressId?: number | null;
    novalogicListing?: boolean;
  } = {},
) {
  return mount(HostedSettingsForm, { props: { previous, products, ...extra } });
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

  it('submits custom network tuning and keeps it when starting a previous server again', async () => {
    start.mockResolvedValue({ ok: true, value: hostedServer(9, { state: 'requested' }) });
    const wrapper = mountForm([JO], settings());
    expect(wrapper.get('#hosted-settings-send_holdoff_ticks-hint').text()).toContain('Nominal 192 ms (5.21 updates/s in each direction)');
    await wrapper.get('#hosted-settings-send_holdoff_ticks').setValue(2);
    await wrapper.get('#hosted-settings-entity_send_budget').setValue(1200);
    await wrapper.get('#hosted-settings-max_packet_size').setValue(1100);
    await wrapper.get('#hosted-settings-lock_framerate').setValue(false);
    expect(wrapper.get('#hosted-settings-send_holdoff_ticks-hint').text()).toContain('Nominal 32 ms (31.25 updates/s in each direction)');
    await wrapper.get('form').trigger('submit');
    await flushPromises();
    const saved = start.mock.calls[0][0] as HostedSettings;
    expect(saved).toMatchObject({ send_holdoff_ticks: 2, entity_send_budget: 1200, max_packet_size: 1100, lock_framerate: false });
    const again = mountForm([JO], saved);
    expect((again.get('#hosted-settings-send_holdoff_ticks').element as HTMLInputElement).value).toBe('2');
    expect((again.get('#hosted-settings-lock_framerate').element as HTMLInputElement).checked).toBe(false);
    await again.get('form').trigger('submit');
    await flushPromises();
    expect(start.mock.calls[1][0]).toEqual(saved);
    wrapper.unmount();
    again.unmount();
  });

  it('opens network tuning for invalid values and marks a field refused by the service', async () => {
    const wrapper = mount(HostedSettingsForm, { props: { previous: settings(), products: [JO] }, attachTo: document.body });
    await wrapper.get('#hosted-settings-max_packet_size').setValue(16385);
    await wrapper.get('form').trigger('submit');
    await flushPromises();
    expect(start).not.toHaveBeenCalled();
    expect((wrapper.get('[data-advanced-settings]').element as HTMLDetailsElement).open).toBe(true);
    expect(document.activeElement?.id).toBe('hosted-settings-max_packet_size');
    expect(wrapper.get('#hosted-settings-max_packet_size-error').text()).toBe('Enter a whole number from 100 to 16384.');
    await wrapper.get('#hosted-settings-max_packet_size').setValue(1300);
    start.mockResolvedValue({ ok: false, error: { code: 'invalid_settings', status: 400, message: 'The server refused the settings.', field: 'send_holdoff_ticks' } });
    await wrapper.get('form').trigger('submit');
    await flushPromises();
    expect(wrapper.get('#hosted-settings-send_holdoff_ticks-error').text()).toBe(FIELD_RULES.send_holdoff_ticks);
    await wrapper.get('#hosted-settings-send_holdoff_ticks').setValue(3);
    expect(wrapper.find('#hosted-settings-send_holdoff_ticks-error').exists()).toBe(false);
    wrapper.unmount();
  });

  it('shows the rules openly and focuses an invalid one', async () => {
    const wrapper = mount(HostedSettingsForm, { props: { previous: settings(), products: [JO] }, attachTo: document.body });
    expect(wrapper.get('#hosted-settings-time_limit').element.closest('details')).toBeNull();
    await wrapper.get('#hosted-settings-time_limit').setValue(999);
    await wrapper.get('form').trigger('submit');
    await flushPromises();
    expect(document.activeElement?.id).toBe('hosted-settings-time_limit');
    expect(start).not.toHaveBeenCalled();
    wrapper.unmount();
  });

  it('protects edited setup until a server start succeeds', async () => {
    start.mockResolvedValue({ ok: true, value: hostedServer(9, { state: 'requested' }) });
    const wrapper = mountForm([JO], settings());
    const isDirty = vi.mocked(useUnsavedChanges).mock.calls.at(-1)![0];
    expect(isDirty()).toBe(false);
    await wrapper.get('#hosted-settings-game_name').setValue('Friday Night');
    expect(isDirty()).toBe(true);
    await wrapper.get('form').trigger('submit');
    await flushPromises();
    expect(isDirty()).toBe(false);
    wrapper.unmount();
  });

  it('offers each game first with its version, and the picked game drives the rest of the form', async () => {
    const wrapper = mountForm();
    const games = radios(wrapper, 'product_id');
    expect(games.map((g) => (g.element as HTMLInputElement).value)).toEqual([String(JO.id), String(DFX2.id)]);
    expect(wrapper.text()).toContain('Delta Force Xtreme 2');
    expect(wrapper.text()).toContain('1.7.5.5');
    // Before any other field.
    const first = wrapper.get('form').element.querySelector('input');
    expect(first?.getAttribute('name')).toBe('hosted-settings-product_id');
    expect((radio(wrapper, 'product_id', String(JO.id)).element as HTMLInputElement).checked).toBe(true);

    // JO: the base game or its expansion, its ranges, every mission of its data
    // by title (the file when the data names none).
    expect(radios(wrapper, 'expansion_id')).toHaveLength(2);
    expect(wrapper.get('#hosted-settings-max_players').attributes('max')).toBe('64');
    expect(wrapper.get('#hosted-settings-max_players-hint').text()).toBe('players, 2 to 64');
    expect(wrapper.get('#hosted-settings-kill_limit-hint').text()).toBe('500 for no limit');
    expect(rotation(wrapper)).toEqual(['River Patrol', 'jo_two.bms']);

    // Its expansion: its missions join the rotation.
    await radio(wrapper, 'expansion_id', String(ESCALATION)).setValue();
    expect(rotation(wrapper)).toEqual(['River Patrol', 'jo_two.bms', 'Escalation Point']);

    // DFX2: no expansion to choose (the form's goes back to the base game), its
    // ranges, its rotation.
    await radio(wrapper, 'product_id', String(DFX2.id)).setValue();
    expect(radios(wrapper, 'expansion_id')).toHaveLength(0);
    expect(wrapper.get('#hosted-settings-max_players').attributes('max')).toBe('50');
    expect(wrapper.get('#hosted-settings-respawn').attributes('max')).toBe('60');
    expect(wrapper.get('#hosted-settings-kill_limit-hint').text()).toBe('500 for no limit');
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
      product_id: DFX2.id,
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
      product_id: JO.id,
      expansion_id: ESCALATION,
      missions: ['jo_one.bms', 'jo_two.bms', 'jox_one.bms'],
    });
  });

  it('lets the rotation hold every mission up to the cap, and no more', async () => {
    const many = Array.from({ length: MISSIONS_MAX + 1 }, (_, i) => ({ file: `m${i}.bms`, title: `Mission ${i}` }));
    const big: HostedProduct = { ...JO, missions: many, expansions: [] };
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
    await wrapper.get('#hosted-settings-missions-remove-0').trigger('click');
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

  it('marks the game picker when the backend refuses the game', async () => {
    start.mockResolvedValue({
      ok: false,
      error: {
        code: 'invalid_settings',
        status: 400,
        message: 'The server refused the settings. Pick a game this service hosts.',
        field: 'product_id',
      },
    });
    const wrapper = mountForm();
    await wrapper.get('#hosted-settings-game_name').setValue('Night Ops');
    await wrapper.get('form').trigger('submit');
    await flushPromises();
    expect(start).toHaveBeenCalledTimes(1);
    expect(start.mock.calls[0][0]).toMatchObject({ product_id: JO.id, expansion_id: null });
    const error = wrapper.get('#hosted-settings-product_id-error');
    expect(error.text()).toBe('Pick a game this service hosts.');
    const picker = wrapper.get('#hosted-settings-product_id').element.closest('fieldset');
    expect(picker?.getAttribute('aria-describedby')).toContain('hosted-settings-product_id-error');
    expect(wrapper.emitted('refused')).toHaveLength(1);
    // Another game takes the mark away.
    await radio(wrapper, 'product_id', String(DFX2.id)).setValue();
    expect(wrapper.find('#hosted-settings-product_id-error').exists()).toBe(false);
  });

  it("starts again from the last server's game, expansion and rotation", async () => {
    const last = settings({ product_id: DFX2.id, max_players: 40, missions: ['dfx_one.bms', 'gone.bms'] });
    const wrapper = mountForm([JO, DFX2], last);
    expect((radio(wrapper, 'product_id', String(DFX2.id)).element as HTMLInputElement).checked).toBe(true);
    expect(rotation(wrapper)).toEqual(['Dust']);
    expect(wrapper.text()).toContain('gone.bms');
    expect((wrapper.get('#hosted-settings-max_players').element as HTMLInputElement).value).toBe('40');
    // Another game: every mission of its data, and nothing said left out.
    await radio(wrapper, 'product_id', String(JO.id)).setValue();
    expect(rotation(wrapper)).toEqual(['River Patrol', 'jo_two.bms']);
    expect(wrapper.text()).not.toContain('gone.bms');

    // The last server's expansion is picked again, with its rotation.
    const withExpansion = mountForm([JO, DFX2], settings({ expansion_id: ESCALATION, missions: ['jox_one.bms'] }));
    expect((radio(withExpansion, 'expansion_id', String(ESCALATION)).element as HTMLInputElement).checked).toBe(true);
    expect(rotation(withExpansion)).toEqual(['Escalation Point']);
  });

  it("offers the listing on NovaLogic's NovaWorld only while the service does, and sends it", async () => {
    start.mockResolvedValue({ ok: true, value: hostedServer(9, { state: 'requested' }) });
    // Not offered: no box, and the start sends it off, even when the last
    // server had it on.
    const unoffered = mountForm([JO], settings({ list_on_novalogic: true }));
    expect(unoffered.find('#hosted-settings-list_on_novalogic').exists()).toBe(false);
    expect(unoffered.text()).not.toContain("NovaLogic's NovaWorld");
    await unoffered.get('form').trigger('submit');
    await flushPromises();
    expect(start.mock.lastCall?.[0]).toMatchObject({ list_on_novalogic: false });

    // Offered: the box, its label all it says, off by default; ticked, it goes on.
    const offered = mountForm([JO], null, { novalogicListing: true });
    const box = offered.get('#hosted-settings-list_on_novalogic');
    expect(box.element.closest('label')?.textContent?.trim()).toBe("Also list on NovaLogic's NovaWorld");
    expect(offered.find('#hosted-settings-list_on_novalogic-hint').exists()).toBe(false);
    expect((box.element as HTMLInputElement).checked).toBe(false);
    await offered.get('#hosted-settings-game_name').setValue('Night Ops');
    await offered.get('form').trigger('submit');
    await flushPromises();
    expect(start.mock.lastCall?.[0]).toMatchObject({ list_on_novalogic: false });
    await box.setValue(true);
    await offered.get('form').trigger('submit');
    await flushPromises();
    expect(start.mock.lastCall?.[0]).toMatchObject({ game_name: 'Night Ops', list_on_novalogic: true });

    // Started again from a server that had it on: ticked from the start.
    const again = mountForm([JO], settings({ list_on_novalogic: true }), { novalogicListing: true });
    expect((again.get('#hosted-settings-list_on_novalogic').element as HTMLInputElement).checked).toBe(true);
  });

  it("shows the NovaLogic box only for a game NovaLogic's NovaWorld lists", () => {
    // Delta Force Xtreme 2 alone: offered by the service, but its game takes none.
    const dfx2 = mountForm([DFX2], null, { novalogicListing: true });
    expect(dfx2.find('#hosted-settings-list_on_novalogic').exists()).toBe(false);
  });

  it("marks the box when the backend refuses the listing on NovaLogic's NovaWorld", async () => {
    start.mockResolvedValue({
      ok: false,
      error: {
        code: 'invalid_settings',
        status: 400,
        message: `The server refused the settings. ${FIELD_RULES.list_on_novalogic}`,
        field: 'list_on_novalogic',
      },
    });
    const wrapper = mountForm([JO], settings({ list_on_novalogic: true }), { novalogicListing: true });
    await wrapper.get('form').trigger('submit');
    await flushPromises();
    expect(wrapper.get('#hosted-settings-list_on_novalogic-error').text()).toBe(FIELD_RULES.list_on_novalogic);
    const box = wrapper.get('#hosted-settings-list_on_novalogic');
    expect(box.attributes('aria-describedby')).toContain('hosted-settings-list_on_novalogic-error');
    expect(wrapper.emitted('refused')).toHaveLength(1);
    // Unticked, the mark goes.
    await box.setValue(false);
    expect(wrapper.find('#hosted-settings-list_on_novalogic-error').exists()).toBe(false);
  });

  it("says so when the last server's game is no longer offered", () => {
    const wrapper = mountForm([JO], settings({ product_id: DFX2.id }));
    expect((radio(wrapper, 'product_id', String(JO.id)).element as HTMLInputElement).checked).toBe(true);
    expect(wrapper.text()).toContain("Your last server's game is no longer hosted");
  });

  it('keeps hints to a few words beside their fields: a counter, the ranges, and the password rule as a tooltip', () => {
    const wrapper = mountForm([JO], null);
    expect(wrapper.get('#hosted-settings-game_name-hint').text()).toMatch(/^\d+\/\d+$/);
    // The counter sits in the label's row, each range beside its field: no line of its own.
    expect(wrapper.get('#hosted-settings-game_name-hint').element.tagName).toBe('SPAN');
    expect(wrapper.get('#hosted-settings-time_limit-hint').element.parentElement!.querySelector('input')).not.toBeNull();
    expect(wrapper.get('#hosted-settings-respawn-hint').text()).toBe('seconds, 0 to 300');
    expect(wrapper.get('#hosted-settings-game_password').attributes('title')).toContain('letters');
    const password = wrapper.get('#hosted-settings-game_password');
    expect(password.attributes('placeholder')).toBe('None: a public server');
    expect(password.attributes('title')).toContain('letters, digits and punctuation');
    expect(wrapper.find('#hosted-settings-game_password-hint').exists()).toBe(false);
    expect(wrapper.text()).not.toContain('pre-1.0');
    expect(wrapper.findAll('h2').map((h) => h.text())).toEqual(['Game', 'Server', 'Rotation', 'Rules']);
    expect(wrapper.get('[data-more-settings] summary').text()).toBe('More settings');
  });

  it('puts the repeat beside the rotation and the other options in More settings', async () => {
    const schema = [
      { key: 'replay', label: 'Repeat rotation', group: 'Rotation', kind: 'boolean' as const, default: true, min: 0, max: 1, command: '', core: false, note: '', depends_on: '' },
      { key: 'punt_vote', label: 'Allow kick votes', group: 'Players and access', kind: 'boolean' as const, default: false, min: 0, max: 1, command: 'PuntVote', core: false, note: '', depends_on: '' },
      { key: 'vote_percent', label: 'Vote threshold', group: 'Players and access', kind: 'number' as const, default: 0.5, min: -2147483648, max: 2147483647, command: 'VotePercent', core: false, note: '', depends_on: 'punt_vote' },
    ];
    start.mockResolvedValue({ ok: true, value: hostedServer(9, { state: 'requested' }) });
    const wrapper = mountForm([{ ...JO, settings_schema: schema }], settings());
    expect(wrapper.find('#hosted-settings-options-replay').exists()).toBe(false);
    await wrapper.get('#hosted-settings-replay').setValue(false);
    expect((wrapper.get('#hosted-settings-options-vote_percent').element as HTMLInputElement).disabled).toBe(true);
    await wrapper.get('#hosted-settings-options-punt_vote').setValue(true);
    expect((wrapper.get('#hosted-settings-options-vote_percent').element as HTMLInputElement).disabled).toBe(false);
    await wrapper.get('form').trigger('submit');
    await flushPromises();
    expect(start.mock.calls[0][0].options).toEqual({ replay: false, punt_vote: true });
  });
});
