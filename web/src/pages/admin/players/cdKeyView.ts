// A CD key's state and maker in words, for the CD keys tab and a player's
// page.

import type { CdKeyState } from '../../../types/cdKeys';
import type { Tone } from '../../../utils/tone';

const STATE_LABELS: Record<CdKeyState, string> = { unassigned: 'Unassigned', assigned: 'Assigned', revoked: 'Revoked' };

export function keyStateLabel(state: CdKeyState): string {
  return STATE_LABELS[state] ?? state;
}

export function keyStateTone(state: CdKeyState): Tone {
  if (state === 'assigned') return 'ok';
  return state === 'revoked' ? 'bad' : 'muted';
}

// Who made a key, from its created_by: "by root", "by the admin token", or
// "at its first activation" (minted then).
export function keyCreatorLabel(createdBy: string): string {
  if (createdBy === 'auto') return 'at its first activation';
  if (createdBy === 'token') return 'by the admin token';
  return createdBy.startsWith('admin ') ? `by ${createdBy.slice(6)}` : createdBy;
}
