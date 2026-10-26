import { describe, expect, it } from 'vitest';
import { bytesOf, novalogicListingText, novalogicListingTone, phaseSlow, phaseText, stoppedBecauseText } from './hostedState';
import type { NovaLogicListing, StoppedBecause } from '../types/hostedServer';

describe('the phase in its owner words', () => {
  it('reads the data downloaded so far of its total in one unit', () => {
    expect(bytesOf(1_200_000_000, 2_300_000_000)).toBe('1.2 of 2.3 GB');
    expect(bytesOf(300_000_000, 600_000_000)).toBe('300 of 600 MB');
    expect(bytesOf(0, 12_000_000_000)).toBe('0 of 12 GB');
    expect(bytesOf(5, 900)).toBe('5 of 900 B');
  });

  it('says what the fleet does for a starting server', () => {
    expect(phaseText({ name: 'downloading_data', bytes_done: 1_200_000_000, bytes_total: 2_300_000_000 })).toBe(
      "Downloading the game's release: 1.2 of 2.3 GB.",
    );
    expect(phaseText({ name: 'downloading_data' })).toBe("Downloading the game's release.");
    expect(phaseText({ name: 'restarting', restart: 2, restart_limit: 3 })).toBe('Restarting after a crash (restart 2 of 3).');
    expect(phaseText({ name: 'restarting', restart: 1, restart_limit: 3, cause: 'crash' })).toBe(
      'Restarting after a crash (restart 1 of 3).',
    );
    // A start that timed out is no crash.
    expect(phaseText({ name: 'restarting', restart: 1, restart_limit: 3, cause: 'timeout' })).toBe(
      'Restarting after it did not come up in time (restart 1 of 3).',
    );
    expect(phaseText({ name: 'restarting', cause: 'timeout' })).toBe('Restarting after it did not come up in time.');
    expect(phaseText({ name: 'moving' })).toBe('Moving to another machine. This is not a restart and does not count toward the restart limit.');
    expect(phaseText({ name: 'data_unavailable' })).toBe("The game's release could not be fetched; the service is retrying.");
    expect(phaseText({ name: 'launching_machine' })).toBe('Starting a machine for it.');
    expect(phaseText({ name: 'waiting_for_address' })).toBe('Waiting for your static address to reach its machine.');
    expect(phaseText({ name: 'running' })).toBe('');
    // Public copy: no em dash anywhere.
    for (const name of ['queued', 'restarting', 'moving', 'launches_held', 'launching_machine', 'waiting_for_slot', 'waiting_for_address', 'downloading_server', 'starting_server', 'signing_in']) {
      expect(phaseText({ name })).not.toContain(String.fromCharCode(0x2014));
    }
  });

  it('calls a phase slow past twice its usual length', () => {
    const now = Date.UTC(2026, 9, 18, 12, 0, 0);
    // Starting a machine usually takes 5 minutes.
    expect(phaseSlow({ phase: { name: 'launching_machine' }, updated_at: '2026-10-18 11:51:00' }, now)).toBe(false);
    expect(phaseSlow({ phase: { name: 'launching_machine' }, updated_at: '2026-10-18 11:49:00' }, now)).toBe(true);
    // So does a static address reaching its machine, timed from the same move into booting.
    expect(phaseSlow({ phase: { name: 'waiting_for_address' }, updated_at: '2026-10-18 11:51:00' }, now)).toBe(false);
    expect(phaseSlow({ phase: { name: 'waiting_for_address' }, updated_at: '2026-10-18 11:49:00' }, now)).toBe(true);
    // The download's grows with the release's size: 2 minutes and 20 MB a second, so
    // 6 GB usually takes 7 minutes and is slow past 14.
    const data = { name: 'downloading_data', bytes_done: 0, bytes_total: 6_000_000_000 };
    expect(phaseSlow({ phase: data, updated_at: '2026-10-18 11:47:00' }, now)).toBe(false);
    expect(phaseSlow({ phase: data, updated_at: '2026-10-18 11:45:00' }, now)).toBe(true);
    // A hold has no usual length; nor has a running server.
    expect(phaseSlow({ phase: { name: 'launches_held' }, updated_at: '2026-10-18 10:00:00' }, now)).toBe(false);
    expect(phaseSlow({ phase: { name: 'running' }, updated_at: '2026-10-18 10:00:00' }, now)).toBe(false);
    expect(phaseSlow({ phase: null, updated_at: '2026-10-18 10:00:00' }, now)).toBe(false);
  });

  it('says why a server stopped', () => {
    const because = (cause: string, extra: Partial<StoppedBecause> = {}): StoppedBecause => ({
      cause,
      limit: null,
      exit_code: null,
      oom: false,
      line: '',
      ...extra,
    });
    expect(stoppedBecauseText(because('crash', { exit_code: 3 }), 'owner')).toBe('It crashed (exit code 3).');
    expect(stoppedBecauseText(because('crash', { exit_code: 137, oom: true }), 'owner')).toBe('It crashed: it ran out of memory.');
    // A start whose data could not be fetched never crashed, whatever docker's code.
    expect(stoppedBecauseText(because('data_unavailable', { exit_code: 125 }), 'owner')).toBe(
      "Its game's release could not be fetched.",
    );
    expect(stoppedBecauseText(because('gone'), 'owner')).toBe('It ended (gone).');
    // Who stopped it: its owner, an admin, or the service (and every older stop).
    expect(stoppedBecauseText(because('owner_stop'), 'owner')).toBe('You stopped it.');
    expect(stoppedBecauseText(because('owner_stop'), 'admin')).toBe('Its owner stopped it.');
    expect(stoppedBecauseText(because('admin_stop'), 'owner')).toBe('An admin stopped it.');
    expect(stoppedBecauseText(because('admin_stop'), 'admin')).toBe('An admin stopped it.');
    expect(stoppedBecauseText(because('stop', { exit_code: 0 }), 'owner')).toBe('It was stopped.');
  });

  it('says which hosting limit stopped a server', () => {
    const limits = (kind: string, count: number): StoppedBecause => ({
      cause: 'limits',
      limit: { kind, count },
      exit_code: null,
      oom: false,
      line: '',
    });
    expect(stoppedBecauseText(limits('shared_servers', 1), 'owner')).toBe('Your shared servers went down to 1, so it was stopped.');
    expect(stoppedBecauseText(limits('shared_servers', 1), 'admin')).toBe("Its owner's shared servers went down to 1, so it was stopped.");
    expect(stoppedBecauseText(limits('no_shared_server', 0), 'owner')).toBe(
      'Your dedicated machines went down to 0 and you had no shared server free for it, so it was stopped.',
    );
    expect(stoppedBecauseText(limits('no_shared_server', 0), 'admin')).toBe(
      "Its owner's dedicated machines went down to 0 and they had no shared server free for it, so it was stopped.",
    );
    expect(stoppedBecauseText(limits('no_shared_room', 1), 'owner')).toBe(
      'Your dedicated machines went down to 1 and the shared machines had no room for it, so it was stopped.',
    );
    // A stop that recorded no limit (an older one), and a limit this site does not know.
    const generic =
      'Your hosting limit no longer covers it (your support tier, dedicated machine or admin grant ended or changed), so it was stopped.';
    expect(stoppedBecauseText({ ...limits('shared_servers', 1), limit: null }, 'owner')).toBe(generic);
    expect(stoppedBecauseText(limits('tier_gone', 1), 'owner')).toBe(generic);
    expect(stoppedBecauseText({ ...limits('shared_servers', 1), limit: null }, 'admin')).toBe(
      "Its owner's hosting limit no longer covers it (their support tier, dedicated machine or admin grant ended or changed), so it was stopped.",
    );
  });
});

describe("the listing on NovaLogic's NovaWorld in its owner's words", () => {
  const NOW = Date.parse('2026-10-25T13:00:00Z');
  const listing = (over: Partial<NovaLogicListing>): NovaLogicListing => ({ state: 'listed', player_slots: [], ...over });

  it('says whether it is listed, and why not, naming NovaLogic for the service', () => {
    expect(novalogicListingText(listing({}), NOW)).toBe("Listed. Players find it in NovaLogic's server browser too.");
    expect(novalogicListingText(listing({ state: 'trying', try: 1, tries: 5 }), NOW)).toBe('Signing in and listing (try 1 of 5).');
    expect(
      novalogicListingText(listing({ state: 'not_listed', reason: 'the service did not accept the server name', tag: 'NWEC57' }), NOW),
    ).toBe('Not listed for this run: NovaLogic did not accept the server name. The next start of the server tries again.');
    expect(novalogicListingText(listing({ state: 'not_listed', reason: 'the service counts too many servers running' }), NOW)).toContain(
      'NovaLogic counts too many servers running',
    );
    // A reason that names no service stays the server's own; the tag never shows.
    expect(novalogicListingText(listing({ state: 'not_listed', reason: 'it did not host in 5 tries; the last: the gate did not answer' }), NOW)).toBe(
      'Not listed for this run: it did not host in 5 tries; the last: the gate did not answer. The next start of the server tries again.',
    );
    expect(novalogicListingText(listing({ state: 'off' }), NOW)).toBe(
      'Not listed: the service does not offer it right now. The next start of the server tries again.',
    );
    expect(novalogicListingText(listing({ state: 'paused' }), NOW)).toBe('');
  });

  it("counts a retry's wait down to its next try", () => {
    const retrying = listing({ state: 'retrying', try: 2, tries: 5, reason: 'the gate did not answer', next_try_at: '2026-10-25T13:02:00Z' });
    expect(novalogicListingText(retrying, NOW)).toBe('Not listed yet: the gate did not answer. It tries again in 2 minutes (try 2 of 5).');
    expect(novalogicListingText(retrying, Date.parse('2026-10-25T13:03:00Z'))).toBe(
      'Not listed yet: the gate did not answer. It tries again shortly (try 2 of 5).',
    );
    expect(novalogicListingText({ ...retrying, next_try_at: undefined }, NOW)).toContain('It tries again shortly');
  });

  it('reads as up, failed for the run, or on its way', () => {
    expect(novalogicListingTone(listing({}))).toBe('good');
    expect(novalogicListingTone(listing({ state: 'not_listed' }))).toBe('bad');
    expect(novalogicListingTone(listing({ state: 'off' }))).toBe('bad');
    expect(novalogicListingTone(listing({ state: 'retrying' }))).toBe('pending');
    expect(novalogicListingTone(listing({ state: 'trying' }))).toBe('pending');
  });
});
