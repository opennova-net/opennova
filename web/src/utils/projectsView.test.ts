import { describe, expect, it } from 'vitest';
import {
  baseGames,
  baseName,
  fieldRule,
  myProjectLink,
  nameProblem,
  projectKindLine,
  suggestKey,
  suggestShortName,
  visibilityLine,
} from './projectsView';
import { ACIDGAMBIT_PROJECT, NIGHTOPS_PROJECT, RANGERS_PROJECT, project } from '../test/productFixtures';

const READABLE = [ACIDGAMBIT_PROJECT, NIGHTOPS_PROJECT, RANGERS_PROJECT];

describe('the projects view', () => {
  it('offers a mod the retail games first, then every game the account may read, by name', () => {
    expect(baseGames(READABLE)).toEqual([
      { key: 'JO', name: 'Joint Operations' },
      { key: 'DFX2', name: 'Delta Force Xtreme 2' },
      { key: 'ACIDGAMBIT', name: 'OP: Acid Gambit' },
      { key: 'RANGERS', name: 'Rangers' },
    ]);
    // An admin reads JO itself: once, by its listed name, in the retail place.
    const jo = project('JO', { title: 'Joint Operations (retail)', owner: null, retail: true });
    expect(baseGames([ACIDGAMBIT_PROJECT, jo]).map((g) => g.name)).toEqual(['Joint Operations (retail)', 'Delta Force Xtreme 2', 'OP: Acid Gambit']);
    expect(baseName('ACIDGAMBIT', READABLE)).toBe('OP: Acid Gambit');
    expect(baseName('GONE', READABLE)).toBe('GONE');
  });

  it('says what a project is', () => {
    expect(projectKindLine(NIGHTOPS_PROJECT, READABLE)).toBe('A mod of Joint Operations');
    expect(projectKindLine(project('X', { kind: 'expansion', base: 'ACIDGAMBIT' }), READABLE)).toBe('A mod of OP: Acid Gambit');
    expect(projectKindLine(RANGERS_PROJECT, [])).toBe('A game for Delta Force Xtreme 2');
    expect(projectKindLine(project('Y', { game: '' }), [])).toBe('A game');
    expect(visibilityLine('public')).toBe('Public: anyone may get it.');
    expect(visibilityLine('private')).toBe("Private: only you and NovaWorld's admins may get it.");
  });

  it('suggests a key and a short name from a name', () => {
    expect(suggestKey('Night Ops: Redux')).toBe('NIGHTOPSREDUX');
    expect(suggestKey('Opération Désert 2 Extended Edition')).toBe('OPERATIONDESERT2');
    expect(suggestShortName('Night Ops: Redux')).toBe('nightops');
    expect(suggestKey('!!!')).toBe('');
  });

  it("checks a name against the routes' rule", () => {
    expect(nameProblem('  ')).toBe('Enter a name.');
    expect(nameProblem('Night Ops')).toBe('');
    expect(nameProblem('x'.repeat(65))).toBe('A name is 1 to 64 characters.');
    expect(nameProblem('Night\u0007Ops')).toBe('A name is 1 to 64 characters.');
    // Characters, not UTF-16 units.
    expect(nameProblem('\u{1F600}'.repeat(64))).toBe('');
  });

  it("names a refused field's rule, and a project's page", () => {
    expect(fieldRule('key')).toBe('A key is 1 to 16 of A-Z, 0-9 and _.');
    expect(fieldRule('expansion_code')).toBe('A short name is 1 to 8 lowercase letters and digits.');
    expect(fieldRule('kind')).toBeNull();
    expect(myProjectLink('NIGHTOPS')).toBe('/me/projects/NIGHTOPS');
  });
});
