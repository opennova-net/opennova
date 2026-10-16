import { describe, expect, it } from 'vitest';
import { DFX2, JO, settings } from '../test/hostedFixtures';
import {
  availableMissions,
  checkSettings,
  DEFAULT_SETTINGS,
  FIELD_RULES,
  fieldRule,
  formFrom,
  initialDataSet,
  initialRotation,
  MISSIONS_MAX,
  missionLabel,
  type SettingsForm,
} from './hostedSettings';

const BOTH = [JO, DFX2];
const ESCALATION = JO.expansions[0].id;

function form(overrides: Partial<SettingsForm> = {}): SettingsForm {
  return { ...formFrom(settings(), JO, ['jo_one.bms']), ...overrides };
}

describe('checkSettings', () => {
  it('passes good settings and sends the data set and expansion by id', () => {
    const result = checkSettings(form({ expansion_id: ESCALATION, missions: ['jo_one.bms', 'jox_one.bms'] }), BOTH);
    expect(result.errors).toEqual({});
    expect(result.settings?.data_set_id).toBe(JO.id);
    expect(result.settings?.expansion_id).toBe(ESCALATION);
    expect(checkSettings(form(), BOTH).settings?.expansion_id).toBeNull();
  });

  it('refuses a game the service does not offer', () => {
    const result = checkSettings(form({ data_set_id: 99 }), BOTH);
    expect(result.settings).toBeNull();
    expect(result.errors.data_set_id).toBe(FIELD_RULES.data_set_id);
    expect(checkSettings(form({ data_set_id: 0 }), BOTH).errors.data_set_id).toBe(FIELD_RULES.data_set_id);
    // Only the offered ones count: JO alone offered, a DFX2 form is refused.
    expect(checkSettings(form({ data_set_id: DFX2.id }), [JO]).errors.data_set_id).toBe(FIELD_RULES.data_set_id);
  });

  it("checks the whole numbers against the picked game's ranges", () => {
    // 60 players fits JO (to 64) but not DFX2 (to 50).
    expect(checkSettings(form({ max_players: 60 }), BOTH).errors.max_players).toBeUndefined();
    const dfx2 = checkSettings(form({ data_set_id: DFX2.id, max_players: 60, missions: ['dfx_one.bms'] }), BOTH);
    expect(dfx2.errors.max_players).toBe('Enter a whole number from 2 to 50.');
    expect(checkSettings(form({ data_set_id: DFX2.id, respawn: 61 }), BOTH).errors.respawn).toBe(
      'Enter a whole number from 0 to 60.',
    );
    expect(checkSettings(form({ time_limit: 0 }), BOTH).errors.time_limit).toBe('Enter a whole number from 1 to 300.');
    expect(checkSettings(form({ kill_limit: '' }), BOTH).errors.kill_limit).toBe('Enter a whole number from 1 to 500.');
    expect(checkSettings(form({ max_score: '2.5' }), BOTH).errors.max_score).toBeDefined();
    // Typed text that holds a whole number counts.
    expect(checkSettings(form({ start_delay: ' 15 ' }), BOTH).settings?.start_delay).toBe(15);
  });

  it("checks the expansion against the picked game's", () => {
    expect(checkSettings(form({ expansion_id: 7 }), BOTH).errors.expansion_id).toBe(
      'The expansion is the base game or Escalation.',
    );
    const dfx2 = checkSettings(form({ data_set_id: DFX2.id, expansion_id: ESCALATION }), BOTH);
    expect(dfx2.errors.expansion_id).toBe('The expansion is the base game.');
    expect(checkSettings(form({ data_set_id: DFX2.id, expansion_id: null, missions: ['dfx_one.bms'] }), BOTH).errors.expansion_id).toBeUndefined();
  });

  it("keeps the rotation to the data's missions, up to the cap", () => {
    expect(checkSettings(form({ missions: [] }), BOTH).errors.missions).toBeDefined();
    expect(checkSettings(form({ missions: ['../up.bms'] }), BOTH).errors.missions).toBeDefined();
    // An expansion's mission needs the expansion.
    expect(checkSettings(form({ missions: ['jox_one.bms'] }), BOTH).errors.missions).toBe(
      '“jox_one.bms” is not a mission of this game and expansion. Pick maps from the list.',
    );
    expect(checkSettings(form({ expansion_id: ESCALATION, missions: ['jox_one.bms'] }), BOTH).errors.missions).toBeUndefined();
    // Matched without regard to case.
    expect(checkSettings(form({ missions: ['JO_ONE.BMS'] }), BOTH).errors.missions).toBeUndefined();
    const many = Array.from({ length: MISSIONS_MAX + 1 }, (_, i) => `m${i}.bms`);
    expect(checkSettings(form({ missions: many }), BOTH).errors.missions).toBe(
      `A rotation holds ${MISSIONS_MAX} missions at most; this one has ${MISSIONS_MAX + 1}.`,
    );
    expect(MISSIONS_MAX).toBe(128);
  });

  it('keeps the name and password rules', () => {
    expect(checkSettings(form({ game_name: ' ' }), BOTH).errors.game_name).toBeDefined();
    expect(checkSettings(form({ game_password: 'has space' }), BOTH).errors.game_password).toBeDefined();
  });
});

describe('fieldRule', () => {
  it("names the game's range and expansions when the game is known", () => {
    expect(fieldRule('max_players', DFX2)).toBe('Max players is 2 to 50.');
    expect(fieldRule('time_limit', JO)).toBe('The time limit is 1 to 300 minutes.');
    expect(fieldRule('expansion_id', JO)).toBe('The expansion is the base game or Escalation.');
    expect(fieldRule('expansion_id', DFX2)).toBe('The expansion is the base game.');
    expect(fieldRule('max_players', null)).toBe(FIELD_RULES.max_players);
    expect(fieldRule('data_set_id', JO)).toBe(FIELD_RULES.data_set_id);
  });
});

describe('the missions the data holds', () => {
  it("lists the base's, then the expansion's, each file once, by title or file", () => {
    expect(availableMissions(JO, null).map((m) => m.file)).toEqual(['jo_one.bms', 'jo_two.bms']);
    expect(availableMissions(JO, ESCALATION).map((m) => m.file)).toEqual(['jo_one.bms', 'jo_two.bms', 'jox_one.bms']);
    expect(availableMissions(null, null)).toEqual([]);
    const twice = { ...JO, expansions: [{ ...JO.expansions[0], missions: [{ file: 'JO_ONE.BMS', title: 'Again' }] }] };
    expect(availableMissions(twice, ESCALATION).map((m) => m.file)).toEqual(['jo_one.bms', 'jo_two.bms']);
    expect(missionLabel({ file: 'jo_one.bms', title: 'River Patrol' })).toBe('River Patrol');
    expect(missionLabel({ file: 'jo_two.bms', title: '' })).toBe('jo_two.bms');
  });
});

describe('the form a start begins with', () => {
  it("picks the last server's game while it is offered, else the first", () => {
    expect(initialDataSet(null, BOTH)).toBe(JO.id);
    expect(initialDataSet(settings({ data_set_id: DFX2.id }), BOTH)).toBe(DFX2.id);
    expect(initialDataSet(settings({ data_set_id: DFX2.id }), [JO])).toBe(JO.id);
    expect(initialDataSet(null, [])).toBe(0);
  });

  it("starts from the defaults or the last server's settings, its expansion only where the game has it", () => {
    const fresh = formFrom(null, DFX2, ['dfx_one.bms']);
    expect(fresh.data_set_id).toBe(DFX2.id);
    expect(fresh.max_players).toBe(DEFAULT_SETTINGS.max_players);
    expect(fresh.expansion_id).toBeNull();
    expect(fresh.missions).toEqual(['dfx_one.bms']);
    expect(formFrom(settings({ expansion_id: ESCALATION }), JO, []).expansion_id).toBe(ESCALATION);
    expect(formFrom(settings({ expansion_id: ESCALATION }), DFX2, []).expansion_id).toBeNull();
    expect(formFrom(settings({ max_players: 12 }), JO, []).max_players).toBe(12);
  });

  it("plays every mission of the data by default, or the last server's for its own game", () => {
    expect(initialRotation(null, JO, null)).toEqual({ missions: ['jo_one.bms', 'jo_two.bms'], dropped: [] });
    expect(initialRotation(null, JO, ESCALATION)).toEqual({ missions: ['jo_one.bms', 'jo_two.bms', 'jox_one.bms'], dropped: [] });
    const last = settings({ missions: ['JO_TWO.BMS', 'gone.bms', 'jo_two.bms'] });
    // Its game: kept to the data's list in the list's spelling, each once, the rest dropped.
    expect(initialRotation(last, JO, null)).toEqual({ missions: ['jo_two.bms'], dropped: ['gone.bms'] });
    // Another game: that game's every mission, nothing said dropped.
    expect(initialRotation(last, DFX2, null)).toEqual({ missions: ['dfx_one.bms'], dropped: [] });
    // Nothing of the last one kept: every mission.
    expect(initialRotation(settings({ missions: ['gone.bms'] }), JO, null)).toEqual({
      missions: ['jo_one.bms', 'jo_two.bms'],
      dropped: ['gone.bms'],
    });
    // The last server ran the expansion; without it, its missions are dropped.
    const withExpansion = settings({ expansion_id: ESCALATION, missions: ['jox_one.bms', 'jo_one.bms'] });
    expect(initialRotation(withExpansion, JO, null)).toEqual({ missions: ['jo_one.bms'], dropped: ['jox_one.bms'] });
    expect(initialRotation(withExpansion, JO, ESCALATION)).toEqual({ missions: ['jox_one.bms', 'jo_one.bms'], dropped: [] });
  });
});
