import { describe, expect, it } from 'vitest';
import {
  baseName,
  fieldRule,
  isOwnedKey,
  myProjectLink,
  nameProblem,
  projectKindLine,
  suggestKey,
  suggestShortName,
  visibilityLine,
} from './projectsView';
import { NIGHTOPS_PROJECT, PROJECT_GAMES, RANGERS_PROJECT, project } from '../test/productFixtures';

describe('the projects view', () => {
  it('names a base game by the games the service lists, else by its key', () => {
    expect(baseName('ACIDGAMBIT', PROJECT_GAMES)).toBe('OP: Acid Gambit');
    expect(baseName('JO', [])).toBe('JO');
    expect(baseName('GONE', PROJECT_GAMES)).toBe('GONE');
  });

  it('says what a project is', () => {
    expect(projectKindLine(NIGHTOPS_PROJECT, PROJECT_GAMES)).toBe('A mod of Joint Operations');
    expect(projectKindLine(project('X', { kind: 'expansion', base: 'ACIDGAMBIT' }), PROJECT_GAMES)).toBe('A mod of OP: Acid Gambit');
    expect(projectKindLine(project('Z', { kind: 'expansion', base: null, game: 'jo' }), [])).toBe('A mod of Joint Operations');
    expect(projectKindLine(RANGERS_PROJECT, [])).toBe('A game for Delta Force Xtreme 2');
    expect(projectKindLine(project('Y', { game: '' }), [])).toBe('A game');
    expect(visibilityLine('public')).toBe('Public: anyone may get it.');
    expect(visibilityLine('private')).toBe("Private: only you and NovaWorld's admins may get it.");
  });

  it('suggests a key and a short name from a name', () => {
    expect(suggestKey('Night Ops: Redux')).toBe('NIGHTOPSRED');
    expect(suggestKey('Opération Désert 2 Extended Edition')).toBe('OPERATIONDE');
    expect(suggestKey('Qqq Squad')).toBe('QSQUAD');
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
    expect(fieldRule('key')).toBe('A key is 1 to 11 of A-Z, 0-9 and _, without "QQ".');
    expect(fieldRule('version')).toBe('A game version is one to four numbers between dots, such as 0.2.0 or 1.0.');
    expect(fieldRule('expansion_code')).toBe('A short name is 1 to 8 lowercase letters and digits.');
    expect(fieldRule('kind')).toBeNull();
    expect(myProjectLink('NIGHTOPS')).toBe('/me/projects/NIGHTOPS');
  });

  it("checks a player's key: 1 to 11 of A-Z, 0-9 and _, with no QQ", () => {
    expect(isOwnedKey('NIGHTOPS')).toBe(true);
    expect(isOwnedKey('A_1')).toBe(true);
    expect(isOwnedKey('ELEVENCHARS')).toBe(true);
    expect(isOwnedKey('TWELVECHARSX')).toBe(false);
    expect(isOwnedKey('')).toBe(false);
    expect(isOwnedKey('IRAQQ')).toBe(false);
    expect(isOwnedKey('night')).toBe(false);
  });
});
