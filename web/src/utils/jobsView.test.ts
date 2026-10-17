import { describe, expect, it } from 'vitest';
import {
  everyLabel,
  groupRetryMessage,
  isCancelled,
  jobKindLabel,
  jobStateLabel,
  leaseLabel,
  scheduleNextLabel,
  secondsLabel,
  utcInputSeconds,
  utcInputValue,
  workerLeaseLabel,
} from './jobsView';
import { NOW, ago, schedule } from '../test/jobsFixtures';

describe('the background jobs in words', () => {
  it('names a schedule’s interval as a person would', () => {
    expect(everyLabel(30)).toBe('every 30 seconds');
    expect(everyLabel(1)).toBe('every second');
    expect(everyLabel(60)).toBe('every minute');
    expect(everyLabel(300)).toBe('every 5 minutes');
    expect(everyLabel(3600)).toBe('every hour');
    expect(everyLabel(7200)).toBe('every 2 hours');
    expect(everyLabel(86_400)).toBe('every day');
    expect(everyLabel(90)).toBe('every 90 seconds');
  });

  it('gives durations in seconds, minutes and hours, never a unit code', () => {
    expect(secondsLabel(0.2)).toBe('less than a second');
    expect(secondsLabel(42)).toBe('42 seconds');
    expect(secondsLabel(125)).toBe('2 minutes');
    expect(secondsLabel(3900)).toBe('1 hour 5 minutes');
    expect(leaseLabel({ lease_left_seconds: 57 })).toBe('runs out in 57 seconds');
    expect(leaseLabel({ lease_left_seconds: -30 })).toBe('ran out 30 seconds ago');
    expect(leaseLabel({ lease_left_seconds: null })).toBe('');
  });

  it('tells an admin’s cancel from a failure, and names known kinds', () => {
    expect(isCancelled({ state: 'failed', last_error: "cancelled by an admin (session 'ops')" })).toBe(true);
    expect(jobStateLabel({ state: 'failed', last_error: "cancelled by an admin (token)" })).toBe('Cancelled');
    expect(jobStateLabel({ state: 'failed', last_error: 'the launch failed' })).toBe('Failed');
    expect(jobStateLabel({ state: 'queued', last_error: '' })).toBe('Waiting');
    expect(jobKindLabel('stats.prune')).toBe("Prune old hosted servers' stats and ended machines");
    expect(jobKindLabel('something.new')).toBe('something.new');
  });

  it('says what a schedule does next', () => {
    expect(scheduleNextLabel(schedule('hosts.prune', { next_at: ago(-600) }), NOW)).toBe('in 10 min');
    expect(scheduleNextLabel(schedule('hosts.prune', { next_at: ago(5) }), NOW)).toBe('Due now');
    expect(scheduleNextLabel(schedule('billing.sync', { next_at: ago(5), conditional: true }), NOW)).toBe('When there is work for it');
    expect(scheduleNextLabel(schedule('jobs.prune', { pending: true }), NOW)).toBe('After the one waiting or running');
  });

  it('reads a datetime-local value as UTC, and writes one back', () => {
    expect(utcInputSeconds('2026-10-16T09:30')).toBe(Date.UTC(2026, 9, 16, 9, 30) / 1000);
    expect(utcInputSeconds('')).toBeNull();
    expect(utcInputSeconds('not a date')).toBeNull();
    expect(utcInputValue(Date.UTC(2026, 9, 16, 9, 30) / 1000)).toBe('2026-10-16T09:30');
  });

  it('says how a worker process holds its leases', () => {
    expect(workerLeaseLabel({ lapsed: 1, lease_left_seconds: 50 })).toBe('1 lease ran out: the process went away');
    expect(workerLeaseLabel({ lapsed: 2, lease_left_seconds: null })).toBe('2 leases ran out: the process went away');
    expect(workerLeaseLabel({ lapsed: 0, lease_left_seconds: 57 })).toBe('Current: the soonest runs out in 57 seconds');
    expect(workerLeaseLabel({ lapsed: 0, lease_left_seconds: null })).toBe('');
  });

  it('says what a failure group’s retry did', () => {
    expect(groupRetryMessage({ retried: 3, secret_dropped: 0, already_pending: 0, changed: 0 })).toBe('3 jobs are queued again to run now.');
    expect(groupRetryMessage({ retried: 1, secret_dropped: 2, already_pending: 1, changed: 1 })).toBe(
      '1 job is queued again to run now. 2 jobs cannot run again: the secret went when it ended. ' +
        '1 job waits behind another with the same key. 1 job had already left the failed state.',
    );
  });
});
