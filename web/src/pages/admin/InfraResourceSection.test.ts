import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import InfraResourceSection from './InfraResourceSection.vue';
import { fetchResourceUsage } from '../../api/usage';
import { NOW, ago, localResourceUsage, resourceUsage } from '../../test/usageFixtures';
import { testRouter } from '../../test/router';

vi.mock('../../api/usage', () => ({ fetchResourceUsage: vi.fn() }));

const read = vi.mocked(fetchResourceUsage);

describe('InfraResourceSection', () => {
  let wrapper: ReturnType<typeof mount> | null = null;

  async function mountPage(path = '/admin/infrastructure/resources/rds') {
    const router = await testRouter(path, ['/admin/infrastructure/resources/:id']);
    wrapper = mount(InfraResourceSection, { global: { plugins: [router] } });
    await flushPromises();
    return wrapper;
  }

  const titles = (page: ReturnType<typeof mount>) => page.findAll('figure figcaption p:first-child').map((p) => p.text());

  beforeEach(() => {
    vi.clearAllMocks();
    vi.useFakeTimers({ toFake: ['Date'] });
    vi.setSystemTime(NOW);
    read.mockResolvedValue({ ok: true, value: resourceUsage() });
  });

  afterEach(() => {
    wrapper?.unmount();
    wrapper = null;
    vi.useRealTimers();
  });

  it("charts the database's metrics, each named in words", async () => {
    const page = await mountPage();
    expect(read).toHaveBeenCalledWith('rds', '48h', expect.anything());
    expect(page.get('h1').text()).toBe('The database (RDS)');
    expect(titles(page)).toEqual(['CPU', 'Free memory', 'Connections']);
    expect(page.text()).toContain("CloudWatch's average each minute.");
  });

  it("shows bytes a period a second, and a target group's healthy hosts by its listener", async () => {
    read.mockResolvedValue({
      ok: true,
      value: resourceUsage({
        id: 'nlb',
        statistics: { ProcessedBytes: 'Sum', 'HealthyHostCount:gate': 'Minimum' },
        metrics: {
          ProcessedBytes: [{ at: ago(60), value: 6_000_000 }],
          'HealthyHostCount:gate': [{ at: ago(60), value: 1 }],
        },
      }),
    });
    const page = await mountPage('/admin/infrastructure/resources/nlb');
    expect(read).toHaveBeenCalledWith('nlb', '48h', expect.anything());
    expect(titles(page)).toEqual(['Traffic', 'Healthy targets: The gate (UDP 7597)']);
    // 6 MB a minute is 100 kB a second.
    expect(page.findAll('figure')[0].text()).toContain('100 kB/s');
  });

  it('says it is not available on the local stack', async () => {
    read.mockResolvedValue({ ok: true, value: localResourceUsage('cache') });
    const page = await mountPage('/admin/infrastructure/resources/cache');
    expect(page.get('.resource-unavailable').text()).toContain('Not available on the local stack.');
    expect(page.get('.resource-unavailable').text()).toContain('AWS is not read here: no ONNET_INFRA_REGION.');
    expect(page.findAll('figure')).toHaveLength(0);
  });

  it("shows AWS's words when CloudWatch could not be read, and reads 400 days on request", async () => {
    read.mockResolvedValue({ ok: true, value: resourceUsage({ error: 'AccessDenied: cloudwatch:GetMetricData', metrics: {} }) });
    const page = await mountPage();
    expect(page.get('.resource-error').text()).toBe('CloudWatch could not be read: AccessDenied: cloudwatch:GetMetricData');
    await page.get('#resource-range-400d').setValue(true);
    await flushPromises();
    expect(read).toHaveBeenLastCalledWith('rds', '400d', expect.anything());
  });
});
