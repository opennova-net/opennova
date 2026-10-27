// The account's projects pages' shared words and lookups
// (pages/account/ProjectsPage.vue, the list; NewProjectPage.vue, a new one;
// ProjectPage.vue, one project): a project's kind in a line, its base game
// named from the games the service lists, the key and short name a name
// suggests, and the routes' rules in words, which the API's refusals reuse
// (api/products.ts).

import type { Project, ProjectGame, ProjectVisibility } from '../types/products';
import { KEY_RULE, titleName } from './productsView';

export { KEY_RULE };

// A mod's short name: 1 to 8 lowercase letters and digits (the routes' rule
// for an expansion's code: its folder, and its archives' name).
export const SHORT_NAME_RULE = /^[a-z0-9]{1,8}$/;
// A name's most characters.
export const NAME_MAX = 64;

export const KEY_HINT = 'A key is 1 to 16 of A-Z, 0-9 and _.';
export const SHORT_NAME_HINT = 'A short name is 1 to 8 lowercase letters and digits.';
export const NAME_HINT = 'A name is 1 to 64 characters.';

// A base game's name by its key, from the games a mod may be for
// (GET /api/me/projects); the key itself when none has it.
export function baseName(key: string, games: readonly ProjectGame[]): string {
  return games.find((g) => g.key === key)?.title ?? key;
}

// "A mod of Joint Operations" or "A game for Joint Operations".
export function projectKindLine(project: Project, games: readonly ProjectGame[]): string {
  if (project.kind === 'expansion') {
    if (project.base) return `A mod of ${baseName(project.base, games)}`;
    return project.game ? `A mod of ${titleName(project.game)}` : 'A mod';
  }
  return project.game ? `A game for ${titleName(project.game)}` : 'A game';
}

export function visibilityLine(visibility: ProjectVisibility): string {
  return visibility === 'public' ? 'Public: anyone may get it.' : "Private: only you and NovaWorld's admins may get it.";
}

// A name's letters and digits, accents dropped.
function plain(name: string): string {
  return name.normalize('NFKD').replace(/[^A-Za-z0-9]/g, '');
}

// The key a name suggests: its letters and digits, upper-cased, the first 16.
export function suggestKey(name: string): string {
  return plain(name).toUpperCase().slice(0, 16);
}

// The short name a name suggests: its letters and digits, lower-cased, the
// first 8.
export function suggestShortName(name: string): string {
  return plain(name).toLowerCase().slice(0, 8);
}

// A name's problem against the routes' rule ('' for none): trimmed, 1 to 64
// characters, no control character.
export function nameProblem(name: string): string {
  const trimmed = name.trim();
  if (trimmed === '') return 'Enter a name.';
  if ([...trimmed].length > NAME_MAX || /[\u0000-\u001f\u007f-\u009f]/.test(trimmed)) return NAME_HINT;
  return '';
}

// What a refused field's rule is, in words; null for a field the page does not
// name.
export function fieldRule(field: string): string | null {
  switch (field) {
    case 'key':
      return KEY_HINT;
    case 'display_name':
      return NAME_HINT;
    case 'expansion_code':
      return SHORT_NAME_HINT;
    case 'base':
      return 'Pick the game the mod is for.';
    case 'game':
      return 'Pick the game it is for.';
    default:
      return null;
  }
}

// A project's page in the account area.
export function myProjectLink(key: string): string {
  return `/me/projects/${encodeURIComponent(key)}`;
}
