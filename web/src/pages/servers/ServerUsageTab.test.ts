import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import ServerUsageTab from './ServerUsageTab.vue';
import { fetchUsage } from '../../api/usage';
import { fleetServer } from '../../test/adminFixtures';
import { NOW, serverUsage } from '../../test/usageFixtures';

vi.mock('../../api/usage', () => ({ fetchUsage: vi.fn() }));

const read = vi.mocked(fetchUsage);

describe('ServerUsageTab', () => {
  let wrapper: ReturnType<typeof mount> | null = null;

  beforeEach(() => {
    vi.clearAllMocks();
    vi.useFakeTimers({ toFake: ['Date'] });
    vi.setSystemTime(NOW);
    read.mockResolvedValue({ ok: true, value: serverUsage() });
  });

  afterEach(() => {
    wrapper?.unmount();
    wrapper = null;
    vi.useRealTimers();
  });

  it("shows the server's usage of its slot and its tick health", async () => {
    wrapper = mount(ServerUsageTab, { props: { server: fleetServer(12), scope: 'admin', now: NOW } });
    await flushPromises();
    expect(read).toHaveBeenCalledWith('server', 12, '48h', { task: undefined, signal: expect.anything() });
    expect(wrapper.get('h2').text()).toBe('Usage');
    expect(wrapper.get('.usage-now').text()).toContain('Server 12: 41 % CPU of 1 vCPU');
    expect(wrapper.get('.usage-now').text()).toContain('3 late ticks in the minute');
    expect(wrapper.text()).toContain('Tick time, 99th percentile');
    expect(wrapper.text()).toContain('Late ticks');
  });
});
