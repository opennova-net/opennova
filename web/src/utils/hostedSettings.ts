// The hosted server settings form: its defaults, and the backend's rules
// (apps/novaworld_server/hosted/hosted_settings.h check_settings) checked in
// the browser first, so the owner hears what is wrong before a start is spent
// on it. The game (a data set the service offers) sets the whole-number
// ranges, the expansions and the rotation's missions. The backend checks them
// again and stays the authority.

import type {
  HostedDataSet,
  HostedExpansion,
  HostedIntField,
  HostedSettings,
  SettingRange,
} from '../types/hostedServer';
import { cp1252Length, cp1252Problem } from './cp1252';

export type SettingsField = keyof HostedSettings;

// The cp1252 bytes a stock host keeps of a name (the host screen's GAME_NAME,
// 32 bytes with its terminator), and the password's (SERVER_PASSWORD, 17).
export const GAME_NAME_MAX = 31;
export const GAME_PASSWORD_MAX = 16;
// A rotation's length, and a mission file name's stem (".bms" besides).
export const MISSIONS_MAX = 32;
export const MISSION_STEM_MAX = 32;

// The whole-number settings, in the form's order. Their ranges are the data
// set's (HostedDataSet.limits, hosted/data_sets.h).
export type IntField = HostedIntField;
export const INT_FIELDS: readonly IntField[] = [
  'max_players',
  'time_limit',
  'kill_limit',
  'max_score',
  'respawn',
  'start_delay',
];

// The backend's defaults for a field a start leaves out (HostedSettings); the
// game's is the first data set the service offers.
export const DEFAULT_SETTINGS: Readonly<HostedSettings> = {
  game: '',
  game_name: '',
  max_players: 32,
  game_password: '',
  time_limit: 30,
  kill_limit: 50,
  max_score: 5,
  respawn: 5,
  start_delay: 0,
  team_ff: false,
  expansion: '',
  missions: [],
};

// The names the page shows for each field.
export const FIELD_LABELS: Record<SettingsField, string> = {
  game: 'Game',
  game_name: 'Server name',
  max_players: 'Max players',
  game_password: 'Game password',
  time_limit: 'Time limit',
  kill_limit: 'Kill limit',
  max_score: 'Max score',
  respawn: 'Respawn time',
  start_delay: 'Pre-round delay',
  team_ff: 'Friendly fire',
  expansion: 'Expansion',
  missions: 'Mission rotation',
};

// Each field's rule in a sentence, for a refusal the backend gave on a field
// (400 invalid_settings names it): never the backend's own text. The ranges
// and the expansions are the game's, so these say so; fieldRule names them
// when the game is known.
export const FIELD_RULES: Record<SettingsField, string> = {
  game: 'Pick a game this service hosts.',
  game_name: `A server name is 1 to ${GAME_NAME_MAX} characters, with no double quote. Use only standard letters, numbers and punctuation.`,
  game_password: `A game password is up to ${GAME_PASSWORD_MAX} plain letters, numbers and punctuation marks, with no space or double quote.`,
  max_players: 'Max players is a whole number in the range the game allows.',
  time_limit: 'The time limit is a whole number of minutes in the range the game allows.',
  kill_limit: 'The kill limit is a whole number in the range the game allows.',
  max_score: 'The max score is a whole number in the range the game allows.',
  respawn: 'The respawn time is a whole number of seconds in the range the game allows.',
  start_delay: 'The pre-round delay is a whole number of seconds in the range the game allows.',
  team_ff: 'Friendly fire is on or off.',
  expansion: 'The expansion is one the game offers.',
  missions: `A rotation is 1 to ${MISSIONS_MAX} maps. Pick maps from the list.`,
};

// A whole-number field's rule with its range.
function rangeRule(field: IntField, { min, max }: SettingRange): string {
  switch (field) {
    case 'max_players':
      return `Max players is ${min} to ${max}.`;
    case 'time_limit':
      return `The time limit is ${min} to ${max} minutes.`;
    case 'kill_limit':
      return `The kill limit is ${min} to ${max}.`;
    case 'max_score':
      return `The max score is ${min} to ${max}.`;
    case 'respawn':
      return `The respawn time is ${min} to ${max} seconds.`;
    case 'start_delay':
      return `The pre-round delay is ${min} to ${max} seconds.`;
  }
}

function isIntField(field: SettingsField): field is IntField {
  return (INT_FIELDS as readonly string[]).includes(field);
}

// A field's rule for the game `dataSet` (null when it is not known), with its
// ranges and its expansions named.
export function fieldRule(field: SettingsField, dataSet: HostedDataSet | null): string {
  if (dataSet && isIntField(field)) return rangeRule(field, dataSet.limits[field]);
  if (dataSet && field === 'expansion') {
    return `The expansion is ${dataSet.expansions.map((e) => e.name).join(' or ')}.`;
  }
  return FIELD_RULES[field];
}

// The offered data set whose games slug is `game`, or null.
export function findDataSet(dataSets: readonly HostedDataSet[], game: string): HostedDataSet | null {
  return dataSets.find((d) => d.game === game) ?? null;
}

// Whether `dataSet` allows expansion `code` ('' is its base game).
export function allowsExpansion(dataSet: HostedDataSet, code: HostedExpansion): boolean {
  return dataSet.expansions.some((e) => e.code === code);
}

// The game a fresh form picks: the last server's while the service still
// offers it, else the service's first; '' when it offers none.
export function initialGame(previous: HostedSettings | null, dataSets: readonly HostedDataSet[]): string {
  if (previous && findDataSet(dataSets, previous.game)) return previous.game;
  return dataSets[0]?.game ?? '';
}

export function isSettingsField(value: unknown): value is SettingsField {
  return typeof value === 'string' && Object.prototype.hasOwnProperty.call(FIELD_LABELS, value);
}

// A .bms file name of letters, digits, '_' and '-' (hosted_settings.cpp
// is_mission_name): 1 to 32 of them, then the extension in any case.
const MISSION_NAME = new RegExp(`^[A-Za-z0-9_-]{1,${MISSION_STEM_MAX}}\\.bms$`, 'i');

export function isMissionName(name: string): boolean {
  return MISSION_NAME.test(name);
}

// Each mission once, in order, the first spelling kept (file names match
// without regard to case): the rotation picker holds each at most once.
export function uniqueMissions(missions: readonly string[]): string[] {
  const seen = new Set<string>();
  const out: string[] = [];
  for (const m of missions) {
    const key = m.toLowerCase();
    if (seen.has(key)) continue;
    seen.add(key);
    out.push(m);
  }
  return out;
}

// The form as typed. A number input bound with v-model.number holds '' when
// empty (or text it could not read), so the whole-number fields take either.
export interface SettingsForm {
  game: string;
  game_name: string;
  max_players: number | string;
  game_password: string;
  time_limit: number | string;
  kill_limit: number | string;
  max_score: number | string;
  respawn: number | string;
  start_delay: number | string;
  team_ff: boolean;
  expansion: HostedExpansion;
  missions: string[];
}

// A fresh form for the game `dataSet`: the given settings (a previous
// server's, to start it again) or the defaults, the expansion kept only when
// the game allows it, with the rotation `missions`.
export function formFrom(
  settings: HostedSettings | null,
  dataSet: HostedDataSet | null,
  missions: string[],
): SettingsForm {
  const base = settings ?? DEFAULT_SETTINGS;
  return {
    game: dataSet?.game ?? '',
    game_name: base.game_name,
    max_players: base.max_players,
    game_password: base.game_password,
    time_limit: base.time_limit,
    kill_limit: base.kill_limit,
    max_score: base.max_score,
    respawn: base.respawn,
    start_delay: base.start_delay,
    team_ff: base.team_ff,
    expansion: dataSet && allowsExpansion(dataSet, base.expansion) ? base.expansion : '',
    missions: [...missions],
  };
}

// The rotation a form starts with for the game `dataSet`: the last server's
// when it ran that game, each mission once (the picker holds each at most
// once) and kept to the game's list when it has one, else the game's own.
// `dropped` names the last server's missions the game no longer offers (or
// that name no .bms file), which the form says it left out.
export function initialRotation(
  previous: HostedSettings | null,
  dataSet: HostedDataSet | null,
): { missions: string[]; dropped: string[] } {
  const offered = uniqueMissions(dataSet?.default_missions ?? []);
  if (!previous || !dataSet || previous.game !== dataSet.game) return { missions: offered, dropped: [] };
  const last = uniqueMissions(previous.missions);
  if (offered.length === 0) {
    return { missions: last.filter(isMissionName), dropped: last.filter((m) => !isMissionName(m)) };
  }
  const known = new Map(offered.map((m) => [m.toLowerCase(), m] as const));
  const kept: string[] = [];
  const dropped: string[] = [];
  for (const m of last) {
    const name = known.get(m.toLowerCase());
    if (name) kept.push(name);
    else dropped.push(m);
  }
  return { missions: kept.length > 0 ? kept : offered, dropped };
}

export type SettingsErrors = Partial<Record<SettingsField, string>>;

// C0 controls, DEL and the C1 controls (U+0080..U+009F), which the backend
// refuses in a name.
const CONTROL = /[\u0000-\u001f\u007f-\u009f]/;

function nameProblem(raw: string): string | null {
  const name = raw.trim();
  if (name === '') return 'Enter a server name.';
  if (name.includes('"')) return 'A server name cannot hold a double quote (").';
  if (CONTROL.test(name)) return 'A server name cannot hold a control character.';
  const unstorable = cp1252Problem(name);
  if (unstorable) return unstorable;
  const length = cp1252Length(name);
  if (length > GAME_NAME_MAX) return `A server name is ${GAME_NAME_MAX} characters at most; this is ${length}.`;
  return null;
}

function passwordProblem(password: string): string | null {
  if (password.length > GAME_PASSWORD_MAX) {
    return `A game password is ${GAME_PASSWORD_MAX} characters at most; this is ${password.length}.`;
  }
  for (const ch of Array.from(password)) {
    const code = ch.codePointAt(0) ?? 0;
    if (code <= 0x20 || code > 0x7e || ch === '"') {
      return 'A game password takes letters, digits and ASCII punctuation, with no space or double quote.';
    }
  }
  return null;
}

// The whole number in a field, or null when it holds none.
function wholeNumber(value: number | string): number | null {
  if (typeof value === 'number') return Number.isInteger(value) ? value : null;
  const text = value.trim();
  if (!/^-?\d+$/.test(text)) return null;
  return Number(text);
}

function missionsProblem(missions: string[]): string | null {
  if (missions.length === 0) return 'Choose at least one mission for the rotation.';
  if (missions.length > MISSIONS_MAX) {
    return `A rotation holds ${MISSIONS_MAX} missions at most; this one has ${missions.length}.`;
  }
  const bad = missions.find((m) => !isMissionName(m));
  if (bad !== undefined) {
    return `“${bad}” is not a map name. Pick maps from the list.`;
  }
  return null;
}

export interface CheckedSettings {
  // Each field's problem; empty when the form is good.
  errors: SettingsErrors;
  // The settings to send, when there is no problem.
  settings: HostedSettings | null;
}

// Every field against the backend's rule, in the form's order: the game one
// of `dataSets`, and the ranges and the expansion that game's own.
export function checkSettings(form: SettingsForm, dataSets: readonly HostedDataSet[]): CheckedSettings {
  const errors: SettingsErrors = {};
  const dataSet = findDataSet(dataSets, form.game);
  if (!dataSet) errors.game = FIELD_RULES.game;
  const name = nameProblem(form.game_name);
  if (name) errors.game_name = name;
  const password = passwordProblem(form.game_password);
  if (password) errors.game_password = password;
  const numbers = {} as Record<IntField, number>;
  for (const field of INT_FIELDS) {
    const value = wholeNumber(form[field]);
    const range = dataSet?.limits[field];
    if (value !== null && (!range || (value >= range.min && value <= range.max))) {
      numbers[field] = value;
    } else {
      errors[field] = range ? `Enter a whole number from ${range.min} to ${range.max}.` : 'Enter a whole number.';
    }
  }
  if (dataSet && !allowsExpansion(dataSet, form.expansion)) errors.expansion = fieldRule('expansion', dataSet);
  const missions = missionsProblem(form.missions);
  if (missions) errors.missions = missions;
  if (Object.keys(errors).length > 0) return { errors, settings: null };
  return {
    errors,
    settings: {
      game: form.game,
      game_name: form.game_name.trim(),
      game_password: form.game_password,
      ...numbers,
      team_ff: form.team_ff,
      expansion: form.expansion,
      missions: [...form.missions],
    },
  };
}
