import type { HostedSettings } from '../types/hostedServer';
import { cp1252Length, cp1252Problem } from './cp1252';

export type SettingValue = string | number | boolean;
export type ServerOptions = Record<string, SettingValue | Record<string, string | boolean>>;
export interface ServerSetting {
  key: string; label: string; group: string;
  kind: 'integer' | 'number' | 'boolean' | 'text' | 'password';
  default: SettingValue; min: number; max: number; command: string; core: boolean; note: string;
}
export interface ServerWeapon { name: string; selectable: boolean }
export interface SavedConfiguration extends Omit<HostedSettings, 'missions' | 'mission_options'> {
  rotation: { file: string; swap_sides: boolean }[];
}

export function configurationDocument(settings: Partial<HostedSettings>): Partial<SavedConfiguration> {
  const { missions, mission_options, ...rest } = settings;
  return { ...rest, ...(missions ? { rotation: missions.map((file, i) => ({ file, swap_sides: mission_options?.[i] === true })) } : {}) };
}

export function settingsFromDocument(settings: SavedConfiguration): HostedSettings {
  const { rotation, ...rest } = settings;
  return { ...rest, missions: rotation.map((entry) => entry.file), mission_options: rotation.map((entry) => entry.swap_sides), options: settings.options ?? {} };
}

export function settingProblem(spec: ServerSetting, value: unknown): string | null {
  if (spec.kind === 'boolean') return typeof value === 'boolean' ? null : 'Choose on or off.';
  if (spec.kind === 'integer' || spec.kind === 'number') {
    return typeof value === 'number' && Number.isFinite(value) && value >= spec.min && value <= spec.max &&
      (spec.kind !== 'integer' || Number.isInteger(value)) ? null : `Enter ${spec.kind === 'integer' ? 'a whole number' : 'a number'} from ${spec.min} to ${spec.max}.`;
  }
  if (typeof value !== 'string') return 'Enter text.';
  if (cp1252Problem(value)) return cp1252Problem(value);
  if (cp1252Length(value) > spec.max) return `Use at most ${spec.max} characters.`;
  if (/[\u0000-\u001f\u007f-\u009f"]/.test(value)) return 'Control characters and double quotes are not supported.';
  if (spec.kind === 'password' && /\s/.test(value)) return 'Passwords cannot contain whitespace.';
  return null;
}

export function parseSettingsSchema(value: unknown): ServerSetting[] {
  if (!Array.isArray(value)) return [];
  return value.filter((s): s is ServerSetting => s && typeof s.key === 'string' && typeof s.label === 'string' &&
    typeof s.group === 'string' && ['integer', 'number', 'boolean', 'text', 'password'].includes(s.kind) &&
    typeof s.min === 'number' && typeof s.max === 'number' && typeof s.core === 'boolean' && typeof s.command === 'string');
}

export function parseWeapons(value: unknown): ServerWeapon[] {
  if (!Array.isArray(value)) return [];
  return value.filter((w): w is ServerWeapon => w && typeof w.name === 'string' && typeof w.selectable === 'boolean');
}
