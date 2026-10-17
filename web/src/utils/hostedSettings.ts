// The hosted server settings form: its defaults, and the backend's rules
// (apps/novaworld_common/hosted/hosted_settings.h check_settings) checked in
// the browser first, so the owner hears what is wrong before a start is spent
// on it. The game (a base data set the service offers, by id) sets the
// whole-number ranges, the expansions and the rotation's missions, which come
// from the data itself (the current trees' mission lists). The backend checks
// them again and stays the authority.

import type { HostedDataSet, HostedExpansionOption, HostedIntField, HostedMission, HostedSettings, SettingRange } from '../types/hostedServer';
import { cp1252Length, cp1252Problem } from './cp1252';

export type SettingsField = keyof HostedSettings;

// The cp1252 bytes a stock host keeps of a name (the host screen's GAME_NAME,
// 32 bytes with its terminator), and the password's (SERVER_PASSWORD, 17).
export const GAME_NAME_MAX = 31;
export const GAME_PASSWORD_MAX = 16;
// A rotation's length (hosted_settings.h kMissionsMax: every mission of a
// base and its expansion fits, more than retail's 32), and a mission file
// name's stem (".bms" besides).
export const MISSIONS_MAX = 128;
export const MISSION_STEM_MAX = 32;

// The whole-number settings, in the form's order. Their ranges are the data
// set's (HostedDataSet.limits, hosted/host_limits.h).
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
// data set's is the first the service offers (0 stands for none here).
export const DEFAULT_SETTINGS: Readonly<HostedSettings> = {
  data_set_id: 0,
  expansion_id: null,
  game_name: '',
  max_players: 32,
  game_password: '',
  time_limit: 30,
  kill_limit: 50,
  max_score: 5,
  respawn: 5,
  start_delay: 0,
  team_ff: false,
  missions: [],
};

// The names the page shows for each field.
export const FIELD_LABELS: Record<SettingsField, string> = {
  data_set_id: 'Game',
  expansion_id: 'Expansion',
  game_name: 'Server name',
  max_players: 'Max players',
  game_password: 'Game password',
  time_limit: 'Time limit',
  kill_limit: 'Kill limit',
  max_score: 'Max score',
  respawn: 'Respawn time',
  start_delay: 'Pre-round delay',
  team_ff: 'Friendly fire',
  missions: 'Mission rotation',
};

// Each field's rule in a sentence, for a refusal the backend gave on a field
// (400 invalid_settings names it): never the backend's own text. The ranges
// and the expansions are the game's, so these say so; fieldRule names them
// when the game is known.
export const FIELD_RULES: Record<SettingsField, string> = {
  data_set_id: 'Pick a game this service hosts.',
  expansion_id: 'The expansion is one the game offers.',
  game_name: `A server name is 1 to ${GAME_NAME_MAX} characters, with no double quote. Use only standard letters, numbers and punctuation.`,
  game_password: `A game password is up to ${GAME_PASSWORD_MAX} plain letters, numbers and punctuation marks, with no space or double quote.`,
  max_players: 'Max players is a whole number in the range the game allows.',
  time_limit: 'The time limit is a whole number of minutes in the range the game allows.',
  kill_limit: 'The kill limit is a whole number in the range the game allows.',
  max_score: 'The max score is a whole number in the range the game allows.',
  respawn: 'The respawn time is a whole number of seconds in the range the game allows.',
  start_delay: 'The pre-round delay is a whole number of seconds in the range the game allows.',
  team_ff: 'Friendly fire is on or off.',
  missions: `A rotation is 1 to ${MISSIONS_MAX} missions of the game and its expansion. Pick them from the list.`,
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

// The expansion choices of a data set, in words: the base game, then each
// expansion by name.
export function expansionChoices(dataSet: HostedDataSet): string {
  return ['the base game', ...dataSet.expansions.map((e) => e.display_name)].join(' or ');
}

// A field's rule for the game `dataSet` (null when it is not known), with its
// ranges and its expansions named.
export function fieldRule(field: SettingsField, dataSet: HostedDataSet | null): string {
  if (dataSet && isIntField(field)) return rangeRule(field, dataSet.limits[field]);
  if (dataSet && field === 'expansion_id') return `The expansion is ${expansionChoices(dataSet)}.`;
  return FIELD_RULES[field];
}

// The offered data set with this id, or null.
export function findDataSet(dataSets: readonly HostedDataSet[], id: number): HostedDataSet | null {
  return dataSets.find((d) => d.id === id) ?? null;
}

// The expansion of `dataSet` with this id, or null (also for null, the base
// game alone).
export function findExpansion(dataSet: HostedDataSet, id: number | null): HostedExpansionOption | null {
  if (id === null) return null;
  return dataSet.expansions.find((e) => e.id === id) ?? null;
}

// Whether `dataSet` allows expansion `id` (null is its base game alone).
export function allowsExpansion(dataSet: HostedDataSet, id: number | null): boolean {
  return id === null || findExpansion(dataSet, id) !== null;
}

// The missions a rotation may hold with `dataSet` and expansion
// `expansionId`: the base's, then the expansion's, each file once (the first
// spelling kept; file names match without regard to case).
export function availableMissions(dataSet: HostedDataSet | null, expansionId: number | null): HostedMission[] {
  if (!dataSet) return [];
  const expansion = findExpansion(dataSet, expansionId);
  const seen = new Set<string>();
  const out: HostedMission[] = [];
  for (const m of [...dataSet.missions, ...(expansion?.missions ?? [])]) {
    const key = m.file.toLowerCase();
    if (seen.has(key)) continue;
    seen.add(key);
    out.push(m);
  }
  return out;
}

// What the page shows for a mission: its title, or its file when the archive
// names none.
export function missionLabel(mission: HostedMission): string {
  return mission.title || mission.file;
}

// The game a fresh form picks: the last server's while the service still
// offers it, else the service's first; 0 when it offers none.
export function initialDataSet(previous: HostedSettings | null, dataSets: readonly HostedDataSet[]): number {
  if (previous && findDataSet(dataSets, previous.data_set_id)) return previous.data_set_id;
  return dataSets[0]?.id ?? 0;
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
  data_set_id: number;
  expansion_id: number | null;
  game_name: string;
  max_players: number | string;
  game_password: string;
  time_limit: number | string;
  kill_limit: number | string;
  max_score: number | string;
  respawn: number | string;
  start_delay: number | string;
  team_ff: boolean;
  missions: string[];
}

// A fresh form for the game `dataSet`: the given settings (a previous
// server's, to start it again) or the defaults, the expansion kept only when
// the game offers it, with the rotation `missions`.
export function formFrom(settings: HostedSettings | null, dataSet: HostedDataSet | null, missions: string[]): SettingsForm {
  const base = settings ?? DEFAULT_SETTINGS;
  return {
    data_set_id: dataSet?.id ?? 0,
    expansion_id: dataSet && allowsExpansion(dataSet, base.expansion_id) ? base.expansion_id : null,
    game_name: base.game_name,
    max_players: base.max_players,
    game_password: base.game_password,
    time_limit: base.time_limit,
    kill_limit: base.kill_limit,
    max_score: base.max_score,
    respawn: base.respawn,
    start_delay: base.start_delay,
    team_ff: base.team_ff,
    missions: [...missions],
  };
}

// The rotation a form starts with for the game `dataSet` and expansion
// `expansionId`: every mission the data offers (the backend's default), or
// the last server's when it ran that game, each once and kept to the offered
// list in the list's spelling. `dropped` names the last server's missions the
// data no longer offers (an expansion's, when the form runs without it),
// which the form says it left out.
export function initialRotation(
  previous: HostedSettings | null,
  dataSet: HostedDataSet | null,
  expansionId: number | null,
): { missions: string[]; dropped: string[] } {
  const offered = availableMissions(dataSet, expansionId).map((m) => m.file);
  if (!previous || !dataSet || previous.data_set_id !== dataSet.id) return { missions: offered, dropped: [] };
  const known = new Map(offered.map((m) => [m.toLowerCase(), m] as const));
  const kept: string[] = [];
  const dropped: string[] = [];
  for (const m of uniqueMissions(previous.missions)) {
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

// `offered`: the missions the data holds for the picked game and expansion
// (null while the game is not known: then only the names' shape is checked).
function missionsProblem(missions: string[], offered: HostedMission[] | null): string | null {
  if (missions.length === 0) return 'Choose at least one mission for the rotation.';
  if (missions.length > MISSIONS_MAX) {
    return `A rotation holds ${MISSIONS_MAX} missions at most; this one has ${missions.length}.`;
  }
  const bad = missions.find((m) => !isMissionName(m));
  if (bad !== undefined) return `“${bad}” is not a map name. Pick maps from the list.`;
  if (offered) {
    const known = new Set(offered.map((m) => m.file.toLowerCase()));
    const unknown = missions.find((m) => !known.has(m.toLowerCase()));
    if (unknown !== undefined) return `“${unknown}” is not a mission of this game and expansion. Pick maps from the list.`;
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
// of `dataSets`, and the ranges, the expansion and the missions that game's
// own.
export function checkSettings(form: SettingsForm, dataSets: readonly HostedDataSet[]): CheckedSettings {
  const errors: SettingsErrors = {};
  const dataSet = findDataSet(dataSets, form.data_set_id);
  if (!dataSet) errors.data_set_id = FIELD_RULES.data_set_id;
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
  const expansionOk = dataSet ? allowsExpansion(dataSet, form.expansion_id) : true;
  if (!expansionOk && dataSet) errors.expansion_id = fieldRule('expansion_id', dataSet);
  const missions = missionsProblem(form.missions, dataSet && expansionOk ? availableMissions(dataSet, form.expansion_id) : null);
  if (missions) errors.missions = missions;
  if (Object.keys(errors).length > 0) return { errors, settings: null };
  return {
    errors,
    settings: {
      data_set_id: form.data_set_id,
      expansion_id: form.expansion_id,
      game_name: form.game_name.trim(),
      game_password: form.game_password,
      ...numbers,
      team_ff: form.team_ff,
      missions: [...form.missions],
    },
  };
}
