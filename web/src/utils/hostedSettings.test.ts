import { describe, expect, it } from 'vitest';
import { DFX2, JO, settings } from '../test/hostedFixtures';
import {
  checkSettings,
  DEFAULT_SETTINGS,
  FIELD_RULES,
  fieldRule,
  formFrom,
  initialGame,
  initialRotation,
  type SettingsForm,
} from './hostedSettings';

const BOTH = [JO, DFX2];

function form(overrides: Partial<SettingsForm> = {}): SettingsForm {
  return { ...formFrom(settings(), JO, ['jo_one.bms']), ...overrides };
}

describe('checkSettings', () => {
  it('passes good settings and sends the game', () => {
    const result = checkSettings(form({ expansion: 'jox01' }), BOTH);
    expect(result.errors).toEqual({});
    expect(result.settings?.game).toBe('jop_2_consumer');
    expect(result.settings?.expansion).toBe('jox01');
  });

  it('refuses a game the service does not offer', () => {
    const result = checkSettings(form({ game: 'opennova_0_consumer' }), BOTH);
    expect(result.settings).toBeNull();
    expect(result.errors.game).toBe(FIELD_RULES.game);
    expect(checkSettings(form({ game: '' }), BOTH).errors.game).toBe(FIELD_RULES.game);
    // Only the offered ones count: JO alone offered, a DFX2 form is refused.
    expect(checkSettings(form({ game: DFX2.game }), [JO]).errors.game).toBe(FIELD_RULES.game);
  });

  it("checks the whole numbers against the picked game's ranges", () => {
    // 60 players fits JO (to 64) but not DFX2 (to 50).
    expect(checkSettings(form({ max_players: 60 }), BOTH).errors.max_players).toBeUndefined();
    const dfx2 = checkSettings(form({ game: DFX2.game, max_players: 60, missions: ['dfx_one.bms'] }), BOTH);
    expect(dfx2.errors.max_players).toBe('Enter a whole number from 2 to 50.');
    expect(checkSettings(form({ game: DFX2.game, respawn: 61 }), BOTH).errors.respawn).toBe(
      'Enter a whole number from 0 to 60.',
    );
    expect(checkSettings(form({ time_limit: 0 }), BOTH).errors.time_limit).toBe('Enter a whole number from 1 to 300.');
    expect(checkSettings(form({ kill_limit: '' }), BOTH).errors.kill_limit).toBe('Enter a whole number from 1 to 500.');
    expect(checkSettings(form({ max_score: '2.5' }), BOTH).errors.max_score).toBeDefined();
    // Typed text that holds a whole number counts.
    expect(checkSettings(form({ start_delay: ' 15 ' }), BOTH).settings?.start_delay).toBe(15);
  });

  it("checks the expansion against the picked game's", () => {
    expect(checkSettings(form({ expansion: 'jox02' }), BOTH).errors.expansion).toBe(
      'The expansion is Joint Operations (base game) or Escalation (jox01).',
    );
    const dfx2 = checkSettings(form({ game: DFX2.game, expansion: 'jox01' }), BOTH);
    expect(dfx2.errors.expansion).toBe('The expansion is Delta Force Xtreme 2 (base game).');
    expect(checkSettings(form({ game: DFX2.game, expansion: '' }), BOTH).errors.expansion).toBeUndefined();
  });

  it('keeps the name, password and rotation rules', () => {
    expect(checkSettings(form({ game_name: ' ' }), BOTH).errors.game_name).toBeDefined();
    expect(checkSettings(form({ game_password: 'has space' }), BOTH).errors.game_password).toBeDefined();
    expect(checkSettings(form({ missions: [] }), BOTH).errors.missions).toBeDefined();
    expect(checkSettings(form({ missions: ['../up.bms'] }), BOTH).errors.missions).toBeDefined();
  });
});

describe('fieldRule', () => {
  it("names the game's range and expansions when the game is known", () => {
    expect(fieldRule('max_players', DFX2)).toBe('Max players is 2 to 50.');
    expect(fieldRule('time_limit', JO)).toBe('The time limit is 1 to 300 minutes.');
    expect(fieldRule('expansion', DFX2)).toBe('The expansion is Delta Force Xtreme 2 (base game).');
    expect(fieldRule('max_players', null)).toBe(FIELD_RULES.max_players);
    expect(fieldRule('game', JO)).toBe(FIELD_RULES.game);
  });
});

describe('the form a start begins with', () => {
  it("picks the last server's game while it is offered, else the first", () => {
    expect(initialGame(null, BOTH)).toBe(JO.game);
    expect(initialGame(settings({ game: DFX2.game }), BOTH)).toBe(DFX2.game);
    expect(initialGame(settings({ game: DFX2.game }), [JO])).toBe(JO.game);
    expect(initialGame(null, [])).toBe('');
  });

  it("starts from the defaults or the last server's settings, its expansion only where the game has it", () => {
    const fresh = formFrom(null, DFX2, DFX2.default_missions);
    expect(fresh.game).toBe(DFX2.game);
    expect(fresh.max_players).toBe(DEFAULT_SETTINGS.max_players);
    expect(fresh.expansion).toBe('');
    expect(fresh.missions).toEqual(['dfx_one.bms']);
    expect(formFrom(settings({ expansion: 'jox01' }), JO, []).expansion).toBe('jox01');
    expect(formFrom(settings({ expansion: 'jox01' }), DFX2, []).expansion).toBe('');
    expect(formFrom(settings({ max_players: 12 }), JO, []).max_players).toBe(12);
  });

  it("keeps the last server's rotation for its own game alone", () => {
    const last = settings({ missions: ['JO_TWO.BMS', 'gone.bms', 'jo_two.bms'] });
    // Its game: kept to the game's list in the list's spelling, each once, the rest dropped.
    expect(initialRotation(last, JO)).toEqual({ missions: ['jo_two.bms'], dropped: ['gone.bms'] });
    // Another game: that game's own rotation, nothing said dropped.
    expect(initialRotation(last, DFX2)).toEqual({ missions: ['dfx_one.bms'], dropped: [] });
    // Nothing of the last one kept: the game's own.
    expect(initialRotation(settings({ missions: ['gone.bms'] }), JO)).toEqual({
      missions: ['jo_one.bms', 'jo_two.bms'],
      dropped: ['gone.bms'],
    });
    // A game with no list keeps the last rotation's well-formed names.
    const open = { ...JO, default_missions: [] };
    expect(initialRotation(settings({ missions: ['a.bms', 'b c.bms'] }), open)).toEqual({
      missions: ['a.bms'],
      dropped: ['b c.bms'],
    });
    expect(initialRotation(null, JO)).toEqual({ missions: ['jo_one.bms', 'jo_two.bms'], dropped: [] });
  });
});
