import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import AttentionList from './AttentionList.vue';
import { fetchAttention } from '../api/attention';
import { NOW, attention, finding } from '../test/usageFixtures';
import { testRouter } from '../test/router';

vi.mock('../api/attention', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../api/attention')>()),
  fetchAttention: vi.fn(),
}));

const read = vi.mocked(fetchAttention);

describe('AttentionList', () => {
  let wrapper: ReturnType<typeof mount> | null = null;

  async function mountList() {
    const router = await testRouter('/admin/overview');
    wrapper = mount(AttentionList, { global: { plugins: [router] } });
    await flushPromises();
    return wrapper;
  }

  beforeEach(() => {
    vi.clearAllMocks();
    vi.useFakeTimers({ toFake: ['Date'] });
    vi.setSystemTime(NOW);
    read.mockResolvedValue({ ok: true, value: attention() });
  });

  afterEach(() => {
    wrapper?.unmount();
    wrapper = null;
    vi.useRealTimers();
  });

  it('lists the findings danger first, each in words and colour, with since when and its page', async () => {
    const page = await mountList();
    const items = page.findAll('li.attention-finding');
    expect(items).toHaveLength(3);
    expect(items[0].text()).toContain('Act now');
    expect(items[0].text()).toContain('machine 3');
    expect(items[0].text()).toContain('the brake holds it');
    expect(items[0].text()).toContain('For 10 minutes');
    expect(items[0].classes()).toContain('border-danger/40');
    expect(items[0].get('a').attributes('href')).toBe('/admin/fleet/machines/3');
    expect(items[1].text()).toContain('Look soon');
    expect(items[2].text()).toContain('Note');
    expect(items[2].find('a').exists()).toBe(false);
    expect(page.find('.attention-empty').exists()).toBe(false);
  });

  it('lists the firing alarms', async () => {
    const page = await mountList();
    const alarm = page.get('li.attention-alarm');
    expect(alarm.text()).toContain('opennova-prod-machines-lost');
    expect(alarm.text()).toContain('A machine was lost. See the Fleet page.');
    expect(alarm.text()).toContain('Firing for 5 minutes');
    expect(page.find('.attention-no-alarms').exists()).toBe(false);
  });

  it('says nothing needs anyone, with only notes left, and that the local stack reads no alarm', async () => {
    read.mockResolvedValue({
      ok: true,
      value: attention({ findings: [finding('info', 'Launches resume at 12:05.', { link: 'https://example.com' })], alarms: [], alarms_read: false }),
    });
    const page = await mountList();
    expect(page.get('.attention-empty').text()).toBe(
      'Nothing needs you: every machine reports and every server is running or starting on time.',
    );
    expect(page.get('.attention-no-alarms').text()).toBe('Alarms are not read here (local stack).');
    // A link that is no page of the site is text.
    const note = page.get('li.attention-finding');
    expect(note.find('a').exists()).toBe(false);
    expect(note.text()).toContain('https://example.com');
  });

  it('keeps the last findings while a read fails, marked as of their time', async () => {
    const page = await mountList();
    read.mockResolvedValue({ ok: false, error: { code: 'network', status: null, message: 'The service could not be reached.' } });
    vi.setSystemTime(NOW + 4 * 60_000);
    await page.findAll('button').find((b) => b.text() === 'Check again')!.trigger('click');
    await flushPromises();
    expect(page.get('.attention-error').text()).toContain('The service could not be reached.');
    expect(page.get('.attention-stale').text()).toBe('Below is the last answer, as of 4 min ago.');
    expect(page.findAll('li.attention-finding')).toHaveLength(3);
    expect(page.find('.attention-empty').exists()).toBe(false);
  });

  it('never says nothing needs anyone while a read fails', async () => {
    read.mockResolvedValue({ ok: true, value: attention({ findings: [], alarms: [] }) });
    const page = await mountList();
    expect(page.find('.attention-empty').exists()).toBe(true);
    read.mockResolvedValue({ ok: false, error: { code: 'network', status: null, message: 'The service could not be reached.' } });
    vi.setSystemTime(NOW + 4 * 60_000);
    await page.findAll('button').find((b) => b.text() === 'Check again')!.trigger('click');
    await flushPromises();
    expect(page.find('.attention-error').exists()).toBe(true);
    expect(page.find('.attention-empty').exists()).toBe(false);
    expect(page.get('.attention-stale').text()).toBe('The last answer found nothing that needed you, as of 4 min ago.');
  });

  it("shows a failed read's reason and checks again on request", async () => {
    read.mockResolvedValue({ ok: false, error: { code: 'db_error', status: 500, message: 'The server hit a database error. Try again shortly.' } });
    const page = await mountList();
    expect(page.get('.attention-error').text()).toContain('The server hit a database error.');
    expect(page.find('.attention-stale').exists()).toBe(false);
    read.mockResolvedValue({ ok: true, value: attention({ findings: [], alarms: [] }) });
    await page.findAll('button').find((b) => b.text() === 'Check again')!.trigger('click');
    await flushPromises();
    expect(page.find('.attention-error').exists()).toBe(false);
    expect(page.find('.attention-stale').exists()).toBe(false);
    expect(page.find('.attention-empty').exists()).toBe(true);
  });
});
