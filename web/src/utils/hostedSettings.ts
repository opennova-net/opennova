// The hosted server settings form: its defaults, and the backend's rules
// (apps/novaworld_server/hosted/hosted_settings.h check_settings) checked in
// the browser first, so the owner hears what is wrong before a start is spent
// on it. The backend checks them again and stays the authority.

import type { HostedExpansion, HostedSettings } from '../types/hostedServer';
import { cp1252Length, cp1252Problem } from './cp1252';

export type SettingsField = keyof HostedSettings;

// The cp1252 bytes a stock host keeps of a name (the host screen's GAME_NAME,
// 32 bytes with its terminator), and the password's (SERVER_PASSWORD, 17).
export const GAME_NAME_MAX = 31;
export const GAME_PASSWORD_MAX = 16;
// A rotation's length, and a mission file name's stem (".bms" besides).
export const MISSIONS_MAX = 32;
export const MISSION_STEM_MAX = 32;

// The whole-number settings' ranges (service policy, hosted_settings.h).
export const INT_RULES = {
  max_players: { min: 2, max: 64 },
  time_limit: { min: 1, max: 300 },
  kill_limit: { min: 1, max: 500 },
  max_score: { min: 1, max: 500 },
  respawn: { min: 0, max: 300 },
  start_delay: { min: 0, max: 300 },
} as const;
export type IntField = keyof typeof INT_RULES;

// The backend's defaults for a field a start leaves out (HostedSettings).
export const DEFAULT_SETTINGS: Readonly<HostedSettings> = {
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
// (400 invalid_settings names it): never the backend's own text.
export const FIELD_RULES: Record<SettingsField, string> = {
  game_name: `A server name is 1 to ${GAME_NAME_MAX} characters of the game's Windows-1252 text, with no double quote or control character.`,
  game_password: `A game password is up to ${GAME_PASSWORD_MAX} printable ASCII characters, with no space or double quote.`,
  max_players: `Max players is ${INT_RULES.max_players.min} to ${INT_RULES.max_players.max}.`,
  time_limit: `The time limit is ${INT_RULES.time_limit.min} to ${INT_RULES.time_limit.max} minutes.`,
  kill_limit: `The kill limit is ${INT_RULES.kill_limit.min} to ${INT_RULES.kill_limit.max}.`,
  max_score: `The max score is ${INT_RULES.max_score.min} to ${INT_RULES.max_score.max}.`,
  respawn: `The respawn time is ${INT_RULES.respawn.min} to ${INT_RULES.respawn.max} seconds.`,
  start_delay: `The pre-round delay is ${INT_RULES.start_delay.min} to ${INT_RULES.start_delay.max} seconds.`,
  team_ff: 'Friendly fire is on or off.',
  expansion: 'The expansion is base Joint Operations or Escalation.',
  missions: `A rotation is 1 to ${MISSIONS_MAX} mission file names of letters, digits, _ and -, each ending in .bms.`,
};

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

// A fresh form: the given settings (a previous server's, to start it again)
// or the defaults, with the rotation `missions` when given.
export function formFrom(settings: HostedSettings | null, missions: string[]): SettingsForm {
  const base = settings ?? DEFAULT_SETTINGS;
  return {
    game_name: base.game_name,
    max_players: base.max_players,
    game_password: base.game_password,
    time_limit: base.time_limit,
    kill_limit: base.kill_limit,
    max_score: base.max_score,
    respawn: base.respawn,
    start_delay: base.start_delay,
    team_ff: base.team_ff,
    expansion: base.expansion === 'jox01' ? 'jox01' : '',
    missions: [...missions],
  };
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
    return `“${bad}” is not a mission file name: up to ${MISSION_STEM_MAX} letters, digits, _ and -, then .bms.`;
  }
  return null;
}

export interface CheckedSettings {
  // Each field's problem; empty when the form is good.
  errors: SettingsErrors;
  // The settings to send, when there is no problem.
  settings: HostedSettings | null;
}

// Every field against the backend's rule, in the form's order.
export function checkSettings(form: SettingsForm): CheckedSettings {
  const errors: SettingsErrors = {};
  const name = nameProblem(form.game_name);
  if (name) errors.game_name = name;
  const password = passwordProblem(form.game_password);
  if (password) errors.game_password = password;
  const numbers = {} as Record<IntField, number>;
  for (const field of Object.keys(INT_RULES) as IntField[]) {
    const { min, max } = INT_RULES[field];
    const value = wholeNumber(form[field]);
    if (value === null || value < min || value > max) {
      errors[field] = `Enter a whole number from ${min} to ${max}.`;
    } else {
      numbers[field] = value;
    }
  }
  if (form.expansion !== '' && form.expansion !== 'jox01') errors.expansion = FIELD_RULES.expansion;
  const missions = missionsProblem(form.missions);
  if (missions) errors.missions = missions;
  if (Object.keys(errors).length > 0) return { errors, settings: null };
  return {
    errors,
    settings: {
      game_name: form.game_name.trim(),
      game_password: form.game_password,
      ...numbers,
      team_ff: form.team_ff,
      expansion: form.expansion,
      missions: [...form.missions],
    },
  };
}
