import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import LogViewer from './LogViewer.vue';
import { fetchLogs } from '../api/logs';
import { logLine, logPage } from '../test/usageFixtures';

vi.mock('../api/logs', () => ({ fetchLogs: vi.fn() }));

const read = vi.mocked(fetchLogs);

describe('LogViewer', () => {
  let wrapper: ReturnType<typeof mount> | null = null;

  async function mountViewer(props: Record<string, unknown> = {}) {
    wrapper = mount(LogViewer, { props: { idBase: 'log', subject: 'server', id: 12, generation: 2, ...props } });
    await flushPromises();
    return wrapper;
  }

  const lineTexts = (page: ReturnType<typeof mount>) => page.findAll('li.log-line').map((l) => l.text());

  beforeEach(() => {
    vi.clearAllMocks();
    read.mockResolvedValue({ ok: true, value: logPage() });
  });

  afterEach(() => {
    wrapper?.unmount();
    wrapper = null;
  });

  it('shows the newest lines oldest first, each with its time, and where they were read', async () => {
    const page = await mountViewer();
    expect(read).toHaveBeenCalledWith({ subject: 'server', id: 12, generation: 2, limit: 200 }, expect.anything());
    expect(page.get('.log-source').text()).toBe('CloudWatch /opennova/prod/servers, stream 12/g2');
    expect(lineTexts(page)).toEqual(['2026-10-18 11:59:00[serve] serving on UDP 32768', '2026-10-18 11:59:30[serve] player joined']);
    expect(page.text()).toContain('2 lines, oldest first');
    expect(page.find('#log-follow').exists()).toBe(true);
  });

  it('loads older lines through the cursor, until the start of the log', async () => {
    const page = await mountViewer();
    read.mockResolvedValueOnce({ ok: true, value: logPage({ lines: [logLine(600, '[serve] starting')], before: null }) });
    await page.get('button.log-older').trigger('click');
    await flushPromises();
    expect(read).toHaveBeenLastCalledWith({ subject: 'server', id: 12, generation: 2, limit: 200, before: 'f/123' });
    expect(lineTexts(page)[0]).toContain('[serve] starting');
    expect(lineTexts(page)).toHaveLength(3);
    expect(page.find('button.log-older').exists()).toBe(false);
    expect(page.text()).toContain('The start of this log');
  });

  it("filters by the route's q, refusing a quote", async () => {
    const page = await mountViewer();
    await page.get('#log-filter').setValue('joined');
    await page.get('form').trigger('submit');
    await flushPromises();
    expect(read).toHaveBeenLastCalledWith({ subject: 'server', id: 12, generation: 2, limit: 200, q: 'joined' }, expect.anything());
    expect(page.text()).toContain('holding "joined"');

    const calls = read.mock.calls.length;
    await page.get('#log-filter').setValue('say "hi"');
    await page.get('form').trigger('submit');
    await flushPromises();
    expect(read.mock.calls.length).toBe(calls);
    expect(page.get('[role="alert"]').text()).toBe('The filter cannot hold a quote or a backslash.');
  });

  it("adds a follow's newer lines after the last one shown", async () => {
    const page = await mountViewer();
    const shown = logPage().lines;
    read.mockResolvedValueOnce({ ok: true, value: logPage({ lines: [shown[1], logLine(5, '[serve] round over')] }) });
    // Off and on again reads at once.
    await page.get('#log-follow').setValue(false);
    await page.get('#log-follow').setValue(true);
    await flushPromises();
    expect(lineTexts(page)).toHaveLength(3);
    expect(lineTexts(page)[2]).toContain('[serve] round over');
    expect(page.find('.log-gap').exists()).toBe(false);
  });

  it("starts over at the newest when more came than a page holds, and says so", async () => {
    const page = await mountViewer();
    read.mockResolvedValueOnce({ ok: true, value: logPage({ lines: [logLine(2, 'much later')], before: 'f/999' }) });
    await page.get('#log-follow').setValue(false);
    await page.get('#log-follow').setValue(true);
    await flushPromises();
    expect(lineTexts(page)).toEqual([expect.stringContaining('much later')]);
    expect(page.get('.log-gap').text()).toContain('load older lines to read the ones between');
  });

  it("offers no follow on a log that no longer grows, and shows the store's refusal", async () => {
    read.mockResolvedValue({
      ok: false,
      error: { code: 'no_log_store', status: 503, message: 'This NovaWorld service reads no log store. The service says: no log store' },
    });
    const page = await mountViewer({ followable: false });
    expect(page.find('#log-follow').exists()).toBe(false);
    expect(page.get('[role="alert"]').text()).toContain('reads no log store');
  });
});
