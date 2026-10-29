import { isAxiosError } from 'axios';
import { adminClient, apiClient } from './client';
import { consolePath, type ConsoleTarget } from './hostedServer';
import { parseSettingsSchema, parseWeapons, type SavedConfiguration, type ServerSetting, type ServerWeapon } from '../utils/serverConfiguration';

export interface ConfigurationState {
  saved: SavedConfiguration; settings_revision: number; schema: ServerSetting[]; weapons: ServerWeapon[];
  settings: { key: string; value: string }[];
}
export interface CommandResult {
  outcome: string; sent: boolean; delivery_unknown: boolean; configuration_saved: boolean; settings_revision: number; replies: string[];
}
const client = (target: ConsoleTarget) => target.kind === 'admin' ? adminClient : apiClient;
export async function readConfiguration(target: ConsoleTarget): Promise<ConfigurationState> {
  const { data } = await client(target).get<ConfigurationState>(consolePath(target, 'settings'));
  if (!data.saved || !Array.isArray(data.saved.rotation) || !Number.isSafeInteger(data.settings_revision)) throw new Error('The server returned an invalid configuration.');
  return { ...data, schema: parseSettingsSchema(data.schema), weapons: parseWeapons(data.weapons) };
}
export async function editConfiguration(target: ConsoleTarget, revision: number, patch: object): Promise<CommandResult> {
  return (await client(target).patch<CommandResult>(consolePath(target, 'settings'), { settings_revision: revision, patch })).data;
}
export async function sendAdminCommand(target: ConsoleTarget, command: string): Promise<CommandResult> {
  return (await client(target).post<CommandResult>(consolePath(target, 'command'), { command })).data;
}
export function configurationError(error: unknown): string {
  if (isAxiosError(error)) {
    if (error.response?.data?.delivery_unknown || !error.response) return 'The result is uncertain. Reload saved settings and live state before retrying; the command may still complete.';
    if (typeof error.response?.data?.message === 'string') return error.response.data.message;
    return `The request failed (HTTP ${error.response?.status}).`;
  }
  return error instanceof Error ? error.message : 'The request failed.';
}
