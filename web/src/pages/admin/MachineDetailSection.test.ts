import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import MachineDetailSection from './MachineDetailSection.vue';
import { fetchHostedFleet, fetchMachine } from '../../api/admin';
import { fetchLogs } from '../../api/logs';
import { fetchUsage } from '../../api/usage';
import { fleetResponse, fleetServer, machine, machineDetail } from '../../test/adminFixtures';
import { NOW, logPage, machineUsage } from '../../test/usageFixtures';
import { testRouter } from '../../test/router';

vi.mock('../../api/admin', () => ({ fetchHostedFleet: vi.fn(), fetchMachine: vi.fn() }));
vi.mock('../../api/usage', () => ({ fetchUsage: vi.fn() }));
vi.mock('../../api/logs', () => ({ fetchLogs: vi.fn() }));

const read = vi.mocked(fetchMachine);
const fleet = vi.mocked(fetchHostedFleet);
const usage = vi.mocked(fetchUsage);
const logs = vi.mocked(fetchLogs);

describe('MachineDetailSection', () => {
  let wrapper: ReturnType<typeof mount> | null = null;

  async function mountPage(path = '/admin/fleet/machines/3') {
    const router = await testRouter(path, ['/admin/fleet/machines/:id']);
    wrapper = mount(MachineDetailSection, { global: { plugins: [router] } });
    await flushPromises();
    return wrapper;
  }

  beforeEach(() => {
    vi.clearAllMocks();
    vi.useFakeTimers({ toFake: ['Date'] });
    vi.setSystemTime(NOW);
    read.mockResolvedValue({ ok: true, value: machineDetail('3') });
    fleet.mockResolvedValue({ ok: true, value: fleetResponse({ servers: [fleetServer(1), fleetServer(5, { machine_ref: '4', slot: 0 })] }) });
    usage.mockResolvedValue({ ok: true, value: machineUsage() });
    logs.mockResolvedValue({ ok: true, value: logPage({ subject: 'machine', id: '3', generation: null, group: '/opennova/prod/machines', stream: 'hostd/hostd/task3' }) });
  });

  afterEach(() => {
    wrapper?.unmount();
    wrapper = null;
    vi.useRealTimers();
  });

  it('shows the machine, its facts against the release, and its last report', async () => {
    const page = await mountPage();
    expect(read).toHaveBeenCalledWith('3', expect.anything());
    expect(page.get('h1').text()).toBe('Machine 3');
    expect(page.get('.machine-report').text()).toBe('Last report 4 s ago');
    const facts = page.get('.machine-facts').text();
    expect(facts).toContain('opennova-hostd@sha256:111111111111');
    expect(facts).toContain("The release's.");
    expect(facts).toContain('ami-0aaa');
    expect(facts).toContain('Version 0.0.20, protocol 3');
    expect(facts).toContain('hostd/hostd/task3');
    expect(page.findAll('.machine-flag')).toHaveLength(0);
  });

  it('says how it differs from the release', async () => {
    read.mockResolvedValue({ ok: true, value: machineDetail('3', { machine: machine('3', { image_id: 'ami-0old', outdated: true }) }) });
    const page = await mountPage();
    expect(page.get('.machine-facts').text()).toContain("Not the release's: it pins ami-0aaa.");
    expect(page.get('.machine-flag').text()).toBe("Older than the release: its machine image ami-0old is not the release's ami-0aaa.");
  });

  it('lists the servers in its slots, its trees with their progress and its timeline', async () => {
    const page = await mountPage();
    // Its slots' servers are its own; the fleet list names the ones it shows.
    const servers = page.findAll('li.machine-server');
    expect(servers).toHaveLength(2);
    expect(servers[0].get('a').attributes('href')).toBe('/admin/fleet/1');
    expect(servers[0].text()).toContain('Server 1');
    expect(servers[0].text()).toContain('Slot 3, port 32771, running');
    expect(servers[1].get('a').attributes('href')).toBe('/admin/fleet/2');
    expect(servers[1].text()).toContain('Server 2');
    expect(servers[1].text()).toContain('Slot 5, port 32773, booting');

    const trees = page.findAll('li.machine-tree');
    expect(trees).toHaveLength(2);
    expect(trees[0].text()).toContain('Ready, 1.2 GB');
    expect(trees[1].text()).toContain('Fetching: 300 MB of 600 MB (50 %), try 2');
    expect(trees[1].get('[role="progressbar"]').attributes('aria-valuenow')).toBe('50');

    const events = page.findAll('li.machine-event').map((e) => e.text());
    expect(events).toHaveLength(4);
    expect(events[0]).toContain('Made: launching');
    expect(events[3]).toContain('Booting to ready: first report');
  });

  it("shows its usage with its disk, and hostd's log", async () => {
    const page = await mountPage();
    expect(usage).toHaveBeenCalledWith('machine', '3', '48h', { task: undefined, signal: expect.anything() });
    expect(page.get('.usage-now').text()).toContain('Machine 3: 22 % CPU');
    expect(page.text()).toContain('Disk free');
    expect(logs).toHaveBeenCalledWith({ subject: 'machine', id: '3', limit: 200 }, expect.anything());
    expect(page.get('.log-source').text()).toBe('CloudWatch /opennova/prod/machines, stream hostd/hostd/task3');
  });

  it('says so when no machine has the id', async () => {
    read.mockResolvedValue({ ok: false, error: { code: 'not_found', status: 404, message: 'It no longer exists. Refresh the list.' } });
    const page = await mountPage('/admin/fleet/machines/99');
    expect(page.get('h1').text()).toBe('No such machine');
    expect(page.get('[role="alert"]').text()).toContain('No fleet machine has this id');
  });
});
