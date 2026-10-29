import { describe, expect, it } from 'vitest';
import { configurationDocument, settingsFromDocument, settingProblem, type SavedConfiguration, type ServerSetting } from './serverConfiguration';
import { settings } from '../test/hostedFixtures';

describe('server configuration', () => {
  it('round trips repeated maps, side flags, and nested restrictions', () => {
    const source = settings({ missions: ['a.bms', 'a.bms', 'b.bms'], mission_options: [false, true, false],
      options: { replay: false, vote_percent: 0.625, classes: { sniper: false }, weapons: { Rifle: 'armory' } } });
    const document = configurationDocument(source) as SavedConfiguration;
    expect(document.rotation[1]).toEqual({ file: 'a.bms', swap_sides: true });
    expect(document).not.toHaveProperty('missions');
    expect(settingsFromDocument(document)).toEqual(source);
  });
  it('validates exact integer and CP1252 limits without coercion', () => {
    const spec: ServerSetting = { key: 'value', label: 'Value', group: 'Rules', kind: 'integer', default: 0,
      min: -1, max: 100, command: 'Test', core: false, note: '', depends_on: '' };
    expect(settingProblem(spec, -1)).toBeNull();
    for (const value of [1.5, '2', NaN, Infinity, 101]) expect(settingProblem(spec, value)).not.toBeNull();
    spec.kind = 'password'; spec.max = 4;
    expect(settingProblem(spec, 'café')).toBeNull();
    for (const value of ['a b', 'a"b', '😀', 'abcde']) expect(settingProblem(spec, value)).not.toBeNull();
  });
});
