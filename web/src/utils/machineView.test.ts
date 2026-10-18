import { describe, expect, it } from 'vitest';
import { eventText, machineFigures, machineFlags, outdatedReasons, reportAge, treeFraction, treeProgress } from './machineView';
import { machine } from '../test/adminFixtures';
import { NOW } from '../test/usageFixtures';

const GIB = 1024 * 1024 * 1024;

describe('reportAge', () => {
  it('counts the seconds since the last report, late past 30', () => {
    expect(reportAge(machine('3'), NOW)).toEqual({ text: 'Last report 4 s ago', late: false });
    expect(reportAge(machine('3', { last_report_at: '2026-10-18T11:59:15Z' }), NOW)).toEqual({ text: 'Last report 45 s ago: late', late: true });
    expect(reportAge(machine('3', { state: 'booting', last_report_at: null }), NOW)).toEqual({ text: 'No report yet: its agent is starting', late: false });
    expect(reportAge(machine('3', { last_report_at: null }), NOW)).toEqual({ text: 'No report yet', late: true });
    expect(reportAge(machine('3', { state: 'ended' }), NOW)).toBeNull();
  });
});

describe('machineFigures', () => {
  it("flags what the health check would, in a word", () => {
    expect(machineFigures(machine('3').stats)).toEqual([
      { label: 'CPU', value: '22 %', flag: '' },
      { label: 'Memory', value: '2 GiB of 4 GiB', flag: '' },
      { label: 'Disk', value: '30 GB free of 40 GB', flag: '' },
      { label: 'Network', value: '300 kB/s in, 1.2 MB/s out', flag: '' },
    ]);
    const pressed = machineFigures({
      ...machine('3').stats!,
      cpu_percent: 91,
      memory_used_bytes: 3.8 * GIB,
      disk_free_bytes: 4_000_000_000,
      net_in_bps: null,
      net_out_bps: null,
    });
    expect(pressed.map((f) => f.flag)).toEqual(['high', 'high', 'low', '']);
    expect(pressed[3].value).toBe('Not measured yet');
    expect(machineFigures(null)).toEqual([]);
  });
});

describe('machineFlags', () => {
  it('says what keeps a machine from its servers, and when it is older than the release', () => {
    expect(machineFlags(machine('3'), NOW)).toEqual([]);
    const flags = machineFlags(
      machine('3', { lost_held: true, protocol_mismatch_since: '2026-10-18T11:55:00Z', agent_protocol: 2, outdated: true, image_id: 'ami-0old' }),
      NOW,
    );
    expect(flags.map((f) => f.bad)).toEqual([true, true, false]);
    expect(flags[0].text).toContain('held by the brake');
    expect(flags[1].text).toBe(
      'Protocol mismatch, held for 5 minutes: its hostd speaks protocol 2, so it takes no server; it drains after 10 minutes.',
    );
    expect(flags[2].text).toBe("Older than the release: its machine image ami-0old is not the release's ami-0aaa.");
    expect(machineFlags(machine('3', { state: 'ended', lost_held: true }), NOW)).toEqual([]);
  });

  it('names each image that differs', () => {
    const m = machine('3', { agent_image: 'ghcr.io/opennova-net/opennova-hostd@sha256:2222', image_id: 'ami-0old' });
    expect(outdatedReasons(m)).toEqual(["its hostd image is not the release's", "its machine image ami-0old is not the release's ami-0aaa"]);
    expect(outdatedReasons(machine('3', { release: { image_id: null, agent_image: null } }))).toEqual([]);
  });
});

describe('the trees and the events', () => {
  it("reads a tree's progress", () => {
    const tree = { tree_id: 12, state: 'fetching', bytes: 300_000_000, total_bytes: 600_000_000, tries: 2, error: null, reported_at: null };
    expect(treeProgress(tree)).toBe('Fetching: 300 MB of 600 MB (50 %), try 2');
    expect(treeFraction(tree)).toBe(0.5);
    expect(treeProgress({ ...tree, total_bytes: null, tries: 1 })).toBe('Fetching: 300 MB so far');
    expect(treeFraction({ ...tree, total_bytes: null })).toBeNull();
    expect(treeProgress({ ...tree, state: 'ready', bytes: 1_200_000_000 })).toBe('Ready, 1.2 GB');
    expect(treeProgress({ ...tree, state: 'failed', tries: 3, error: 'the object store refused it' })).toBe(
      'Failed after 3 tries: the object store refused it',
    );
  });

  it('tells each event', () => {
    expect(eventText({ from: null, to: 'launching', detail: '', at: '' })).toBe('Made: launching');
    expect(eventText({ from: 'booting', to: 'ready', detail: 'first report', at: '' })).toBe('Booting to ready: first report');
    expect(eventText({ from: 'ready', to: 'ready', detail: 'log stream hostd/hostd/abc', at: '' })).toBe('log stream hostd/hostd/abc');
  });
});
