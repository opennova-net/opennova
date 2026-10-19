import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import ServerLogsTab from './ServerLogsTab.vue';
import { fetchLogs } from '../../api/logs';
import { fleetServer } from '../../test/adminFixtures';
import { NOW, logPage } from '../../test/usageFixtures';

vi.mock('../../api/logs', () => ({ fetchLogs: vi.fn() }));

const read = vi.mocked(fetchLogs);

describe('ServerLogsTab', () => {
  let wrapper: ReturnType<typeof mount> | null = null;

  beforeEach(() => {
    vi.clearAllMocks();
    read.mockResolvedValue({ ok: true, value: logPage({ id: '12', generation: 3, stream: '12/g3' }) });
  });

  afterEach(() => {
    wrapper?.unmount();
    wrapper = null;
  });

  it('reads the newest start, and an older one picked, following only the newest', async () => {
    wrapper = mount(ServerLogsTab, { props: { server: fleetServer(12, { generation: 3 }), scope: 'admin', now: NOW } });
    await flushPromises();
    const options = wrapper.findAll('option').map((o) => o.text());
    expect(options).toEqual(['Start 3, the newest', 'Start 2', 'Start 1']);
    expect(read).toHaveBeenCalledWith({ subject: 'server', id: 12, generation: 3, limit: 200 }, expect.anything());
    expect(wrapper.find('#hosted-server-12-log-follow').exists()).toBe(true);

    await wrapper.get('select').setValue('1');
    await flushPromises();
    expect(read).toHaveBeenLastCalledWith({ subject: 'server', id: 12, generation: 1, limit: 200 }, expect.anything());
    expect(wrapper.text()).toContain("Start 1's output");
    // An older start no longer grows.
    expect(wrapper.find('#hosted-server-12-log-follow').exists()).toBe(false);
  });

  it('says a server that never started has no log', async () => {
    wrapper = mount(ServerLogsTab, { props: { server: fleetServer(12, { generation: 0, state: 'requested' }), scope: 'admin', now: NOW } });
    await flushPromises();
    expect(wrapper.text()).toContain('The server has not started yet, so it has no log.');
    expect(read).not.toHaveBeenCalled();
  });
});
