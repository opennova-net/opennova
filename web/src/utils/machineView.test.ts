import { describe, expect, it } from 'vitest';
import { eventText, gameShortName, machineFigures, machineFlags, outdatedReasons, releaseMatch, reportAge, slotCell, treeFraction, treeProgress } from './machineView';
import { fleetServer, machine } from '../test/adminFixtures';
import { NOW } from '../test/usageFixtures';

const GIB = 1024 * 1024 * 1024;

describe('reportAge', () => {
  it('counts the seconds since the last report, late past 30', () => {
    expect(reportAge(machine('3'), NOW)).toEqual({ text: 'Last report 4 s ago', late: false });
    expect(reportAge(machine('3', { last_report_at: '2026-10-18T11:59:15Z' }), NOW)).toEqual({ text: 'Last report 45 s ago: late', late: true });
    expect(reportAge(machine('3', { state: 'booting', last_report_at: null }), NOW)).toEqual({ text: 'No report yet', late: false });
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
    expect(pressed[3].value).toBe('—');
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
    expect(flags[0].text).toBe('Lost, held by the brake');
    expect(flags[1].text).toBe('Agent protocol 2 mismatch, held 5 minutes');
    expect(flags[2].text).toBe('Older than the release: machine image ami-0old');
    expect(machineFlags(machine('3', { state: 'ended', lost_held: true }), NOW)).toEqual([]);
  });

  it('names each image that differs', () => {
    const m = machine('3', { agent_image: 'ghcr.io/opennova-net/opennova-hostd@sha256:2222', image_id: 'ami-0old' });
    expect(outdatedReasons(m)).toEqual(['hostd image', 'machine image ami-0old']);
    expect(outdatedReasons(machine('3', { release: { image_id: null, agent_image: null } }))).toEqual([]);
  });
});

describe('the slots', () => {
  it('names the server in a slot: its name, its game, its players and its state', () => {
    const servers = [fleetServer(1, { players: 5, tick: { p99_ms: 31, late: 2 } }), fleetServer(2, { state: 'booting', players: null })];
    expect(slotCell(0, null, servers)).toMatchObject({ word: 'Free', tone: 'muted', name: '' });
    expect(slotCell(3, { slot: 3, server_id: 1, state: 'running' }, servers)).toMatchObject({
      name: 'Server 1', game: 'JO', players: '5/32', word: 'Late', tone: 'warn',
    });
    expect(slotCell(5, { slot: 5, server_id: 2, state: 'booting' }, servers)).toMatchObject({ name: 'Server 2', players: '', word: 'Starting' });
    // The holder's own name (the machine's page) over the fleet's.
    expect(slotCell(1, { slot: 1, server_id: 1, state: 'running', name: 'Ranked TDM', tick: { p99_ms: 12, late: 0 } }, servers)).toMatchObject({ name: 'Ranked TDM', word: 'Running', tone: 'ok' });
    expect(slotCell(2, { slot: 2, server_id: 150, state: 'failed' }, [])).toMatchObject({ name: 'Server 150', game: '', word: 'Failed', tone: 'bad' });
  });

  it("calls a game by its key, else its title's name", () => {
    expect(gameShortName(fleetServer(1))).toBe('JO');
    expect(gameShortName(fleetServer(1, { product: { id: 3, key: '', title: 'dfx2', display_name: 'DFX2' } }))).toBe('Delta Force Xtreme 2');
    expect(gameShortName(fleetServer(1, { product: null }))).toBe('');
  });

  it("says whether a fact is the release's", () => {
    expect(releaseMatch('ami-0aaa', 'ami-0aaa')).toEqual({ text: "Release's", tone: 'ok' });
    expect(releaseMatch('ami-0old', 'ami-0aaa')).toEqual({ text: 'Not the release', tone: 'warn' });
    expect(releaseMatch(null, 'ami-0aaa').text).toBe('');
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
