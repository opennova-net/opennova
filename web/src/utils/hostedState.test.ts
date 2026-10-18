import { describe, expect, it } from 'vitest';
import { bytesOf, phaseSlow, phaseText, stoppedBecauseText } from './hostedState';

describe('the phase in its owner words', () => {
  it('reads the data downloaded so far of its total in one unit', () => {
    expect(bytesOf(1_200_000_000, 2_300_000_000)).toBe('1.2 of 2.3 GB');
    expect(bytesOf(300_000_000, 600_000_000)).toBe('300 of 600 MB');
    expect(bytesOf(0, 12_000_000_000)).toBe('0 of 12 GB');
    expect(bytesOf(5, 900)).toBe('5 of 900 B');
  });

  it('says what the fleet does for a starting server', () => {
    expect(phaseText({ name: 'downloading_data', bytes_done: 1_200_000_000, bytes_total: 2_300_000_000 })).toBe(
      'Downloading game data: 1.2 of 2.3 GB.',
    );
    expect(phaseText({ name: 'downloading_data' })).toBe('Downloading game data.');
    expect(phaseText({ name: 'restarting', restart: 2, restart_limit: 3 })).toBe('Restarting after a crash (restart 2 of 3).');
    expect(phaseText({ name: 'moving' })).toBe('Moving to another machine. This is not a restart and does not count toward the restart limit.');
    expect(phaseText({ name: 'launching_machine' })).toBe('Starting a machine for it.');
    expect(phaseText({ name: 'running' })).toBe('');
    // Public copy: no em dash anywhere.
    for (const name of ['queued', 'restarting', 'moving', 'launches_held', 'launching_machine', 'waiting_for_slot', 'downloading_server', 'starting_server', 'signing_in']) {
      expect(phaseText({ name })).not.toContain('—');
    }
  });

  it('calls a phase slow past twice its usual length', () => {
    const now = Date.UTC(2026, 9, 18, 12, 0, 0);
    // Starting a machine usually takes 5 minutes.
    expect(phaseSlow({ phase: { name: 'launching_machine' }, updated_at: '2026-10-18 11:51:00' }, now)).toBe(false);
    expect(phaseSlow({ phase: { name: 'launching_machine' }, updated_at: '2026-10-18 11:49:00' }, now)).toBe(true);
    // The game data's grows with its size: 2 minutes and 20 MB a second.
    const data = { name: 'downloading_data', bytes_done: 0, bytes_total: 6_000_000_000 };
    expect(phaseSlow({ phase: data, updated_at: '2026-10-18 11:45:00' }, now)).toBe(false);
    expect(phaseSlow({ phase: data, updated_at: '2026-10-18 11:40:00' }, now)).toBe(true);
    // A hold has no usual length; nor has a running server.
    expect(phaseSlow({ phase: { name: 'launches_held' }, updated_at: '2026-10-18 10:00:00' }, now)).toBe(false);
    expect(phaseSlow({ phase: { name: 'running' }, updated_at: '2026-10-18 10:00:00' }, now)).toBe(false);
    expect(phaseSlow({ phase: null, updated_at: '2026-10-18 10:00:00' }, now)).toBe(false);
  });

  it('says why a server stopped', () => {
    expect(stoppedBecauseText({ cause: 'crash', exit_code: 3, oom: false, line: '' })).toBe('It crashed (exit code 3).');
    expect(stoppedBecauseText({ cause: 'crash', exit_code: 137, oom: true, line: '' })).toBe('It crashed: it ran out of memory.');
    expect(stoppedBecauseText({ cause: 'stop', exit_code: 0, oom: false, line: '' })).toBe('It was stopped.');
    expect(stoppedBecauseText({ cause: 'limits', exit_code: null, oom: false, line: '' })).toContain('Your hosting limit no longer covers it');
    expect(stoppedBecauseText({ cause: 'gone', exit_code: null, oom: false, line: '' })).toBe('It ended (gone).');
  });
});
