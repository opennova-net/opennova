import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import OverviewSection from './OverviewSection.vue';
import { fetchConnections, fetchHostedFleet, fetchServerStatus } from '../../api/admin';
import { fetchAdminBilling } from '../../api/adminBilling';
import { fetchAttention } from '../../api/attention';
import { fetchTiers } from '../../api/billing';
import { fetchCosts } from '../../api/costs';
import { fetchFleetStats } from '../../api/hostedStats';
import { fetchJobsOverview } from '../../api/jobs';
import { adminBilling, fleetResponse } from '../../test/adminFixtures';
import { tiersResponse } from '../../test/billingFixtures';
import { costs, localCosts } from '../../test/costFixtures';
import { overview } from '../../test/jobsFixtures';
import { serverStats } from '../../test/statsFixtures';
import { attention } from '../../test/usageFixtures';
import { testRouter } from '../../test/router';
import { ADMIN_AREAS } from '../../utils/adminNav';

vi.mock('../../api/admin', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../../api/admin')>()),
  fetchHostedFleet: vi.fn(),
  fetchServerStatus: vi.fn(),
  fetchConnections: vi.fn(),
}));
vi.mock('../../api/adminBilling', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../../api/adminBilling')>()),
  fetchAdminBilling: vi.fn(),
}));
vi.mock('../../api/attention', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../../api/attention')>()),
  fetchAttention: vi.fn(),
}));
vi.mock('../../api/billing', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../../api/billing')>()),
  fetchTiers: vi.fn(),
}));
vi.mock('../../api/costs', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../../api/costs')>()),
  fetchCosts: vi.fn(),
}));
vi.mock('../../api/hostedStats', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../../api/hostedStats')>()),
  fetchFleetStats: vi.fn(),
}));
vi.mock('../../api/jobs', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../../api/jobs')>()),
  fetchJobsOverview: vi.fn(),
}));

const fleet = vi.mocked(fetchHostedFleet);
const status = vi.mocked(fetchServerStatus);
const sessions = vi.mocked(fetchConnections);
const billing = vi.mocked(fetchAdminBilling);
const needs = vi.mocked(fetchAttention);
const tiers = vi.mocked(fetchTiers);
const aws = vi.mocked(fetchCosts);
const stats = vi.mocked(fetchFleetStats);
const jobs = vi.mocked(fetchJobsOverview);

type Page = ReturnType<typeof mount>;

function area(page: Page, label: string) {
  return page.findAll('.overview-area').find((a) => a.get('h3').text() === label)!;
}

// A figure's text in an area: its label, its value and its detail.
function figure(page: Page, areaLabel: string, label: string): string {
  return area(page, areaLabel)
    .findAll('.overview-figure')
    .find((f) => f.get('dt').text() === label)!
    .text();
}

describe('OverviewSection', () => {
  let wrapper: Page | null = null;

  async function mountOverview() {
    const router = await testRouter('/admin/overview');
    wrapper = mount(OverviewSection, { global: { plugins: [router] } });
    await flushPromises();
    return wrapper;
  }

  beforeEach(() => {
    vi.clearAllMocks();
    fleet.mockResolvedValue({ ok: true, value: fleetResponse() });
    status.mockResolvedValue({ ok: true, value: { maintenance_enabled: false, message: '' } });
    sessions.mockResolvedValue({
      ok: true,
      value: {
        count: 2,
        heartbeat_timeout_ms: 240_000,
        connections: [
          { session_id: 1, lobby_id: 1, id: 1, state: 'active', pn: 'NOVAWORLDUDP', identity: '', created_ms: 0, last_seen_ms: 0, addr: '', account: null, host: null },
          {
            session_id: 2,
            lobby_id: 1,
            id: 2,
            state: 'active',
            pn: 'JOINTOPERATIONS',
            identity: '',
            created_ms: 0,
            last_seen_ms: 0,
            addr: '',
            account: null,
            host: { rid: 9, server_name: 'Ops', mission_name: '', players: 0, max_players: 16, listed: true, hosted_server_id: null },
          },
        ],
      },
    });
    billing.mockResolvedValue({ ok: true, value: adminBilling() });
    needs.mockResolvedValue({ ok: true, value: attention() });
    tiers.mockResolvedValue({ ok: true, value: tiersResponse() });
    aws.mockResolvedValue({ ok: true, value: costs() });
    stats.mockResolvedValue({ ok: true, value: serverStats() });
    jobs.mockResolvedValue({ ok: true, value: overview() });
  });

  afterEach(() => {
    wrapper?.unmount();
    wrapper = null;
  });

  it('puts what needs you first, then each area at a glance', async () => {
    const page = await mountOverview();
    expect(page.get('h1').text()).toBe('Overview');
    // No breadcrumb on the home page.
    expect(page.find('nav[aria-label="Breadcrumb"]').exists()).toBe(false);
    const html = page.html();
    expect(html.indexOf('Needs you')).toBeGreaterThan(-1);
    expect(html.indexOf('Needs you')).toBeLessThan(html.indexOf('At a glance'));
    expect(page.findAll('.overview-area h3').map((h) => h.text())).toEqual(['NovaWorld', 'Hosting', 'People and money', 'Operations']);
  });

  it("states each area in words beside the colour: the worst of what it holds, what needs you among it", async () => {
    const page = await mountOverview();
    const badge = (label: string) => area(page, label).get('.overview-area-status');
    expect(badge('NovaWorld').text()).toBe('Open');
    expect(badge('NovaWorld').classes()).toContain('text-online');
    // Two of the findings link to the fleet's machines, one of them danger.
    expect(badge('Hosting').text()).toBe('2 need you');
    expect(badge('Hosting').classes()).toContain('text-danger');
    expect(area(page, 'Hosting').classes()).toContain('border-danger/60');
    // A payment past due, the budget well within.
    expect(badge('People and money').text()).toBe('1 payment past due');
    expect(badge('People and money').classes()).toContain('text-warn');
    // The firing alarm.
    expect(badge('Operations').text()).toBe('1 needs you');
    expect(badge('Operations').classes()).toContain('text-danger');
  });

  it('shows the key numbers of each area', async () => {
    const page = await mountOverview();
    expect(figure(page, 'NovaWorld', 'Sign-in')).toContain('Open');
    expect(figure(page, 'NovaWorld', 'Signed in now')).toBe('Signed in now21 hosts a server');
    expect(figure(page, 'Hosting', 'Live servers')).toContain('1 of 24 shared');
    expect(figure(page, 'Hosting', 'Live servers')).toContain('0 of 2 own');
    expect(figure(page, 'Hosting', 'Machines')).toContain('1 shared, 1 own');
    expect(figure(page, 'Hosting', 'Players on hosted servers')).toContain('5');
    expect(area(page, 'Hosting').get('svg').attributes('aria-label')).toContain('Players on hosted servers, last 24 hours');
    expect(figure(page, 'People and money', 'Subscribers')).toContain('Tier 1: 1, Tier 2: 2, Tier 3: 1');
    expect(figure(page, 'People and money', 'Payments past due')).toContain('Their servers keep running while Stripe retries');
    // Four tiers and two own machines at today's prices.
    expect(figure(page, 'People and money', 'Subscriptions a month')).toContain('about $130');
    expect(figure(page, 'People and money', 'AWS this month')).toContain('$41.25 so far');
    expect(figure(page, 'People and money', 'AWS this month')).toContain("About $117 by the month's end, 47% of the $250 budget.");
    expect(figure(page, 'Operations', 'Alarms firing')).toContain('opennova-prod-machines-lost');
    expect(figure(page, 'Operations', 'Background jobs')).toContain('3 queued, 2 running');
    expect(figure(page, 'Operations', 'Background jobs')).toContain('16 failed in the last day');
  });

  it("links into every page of each area, saying what it holds", async () => {
    const page = await mountOverview();
    for (const a of ADMIN_AREAS) {
      const links = area(page, a.label).findAll('ul a').map((l) => [l.text(), l.attributes('href')]);
      expect(links).toEqual(a.sections.map((s) => [s.label, s.to]));
    }
    expect(area(page, 'People and money').text()).toContain("The month's AWS bill against the budget");
  });

  it('marks maintenance, and an AWS bill near the budget', async () => {
    status.mockResolvedValue({ ok: true, value: { maintenance_enabled: true, message: 'Back soon' } });
    billing.mockResolvedValue({ ok: true, value: adminBilling({ subscriptions: [] }) });
    aws.mockResolvedValue({ ok: true, value: costs({ forecast_cents: 26000 }) });
    const page = await mountOverview();
    expect(area(page, 'NovaWorld').get('.overview-area-status').text()).toBe('In maintenance');
    expect(figure(page, 'NovaWorld', 'Sign-in')).toContain('Game clients cannot sign in');
    expect(area(page, 'People and money').get('.overview-area-status').text()).toBe('Near the AWS budget');
    expect(figure(page, 'People and money', 'AWS this month')).toContain("About $260 by the month's end, 104% of the $250 budget.");
  });

  it('names a read that failed, and leaves the rest', async () => {
    aws.mockResolvedValue({ ok: false, error: { code: 'network', status: null, message: 'Could not reach the server.' } });
    jobs.mockResolvedValue({ ok: false, error: { code: 'db_error', status: 500, message: 'The server hit a database error.' } });
    const page = await mountOverview();
    expect(page.get('.overview-problems').text()).toBe(
      'Not read: the background jobs (The server hit a database error.); AWS costs (Could not reach the server.).',
    );
    expect(figure(page, 'People and money', 'AWS this month')).toContain('Unknown');
    expect(area(page, 'Operations').get('.overview-area-status').text()).toBe('1 needs you');
    expect(figure(page, 'Operations', 'Background jobs')).toContain('Not read');
    expect(figure(page, 'Hosting', 'Live servers')).toContain('1 of 24 shared');
  });

  it('says the local stack reads no AWS bill, and when billing is off', async () => {
    aws.mockResolvedValue({ ok: true, value: localCosts() });
    billing.mockResolvedValue({ ok: true, value: adminBilling({ enabled: false }) });
    needs.mockResolvedValue({ ok: true, value: attention({ findings: [], alarms: [], alarms_read: false }) });
    const page = await mountOverview();
    expect(figure(page, 'People and money', 'AWS this month')).toContain('Not read here');
    expect(figure(page, 'People and money', 'Subscribers')).toContain('Billing off');
    expect(area(page, 'People and money').find('.overview-figure dt').exists()).toBe(true);
    expect(area(page, 'People and money').text()).not.toContain('Subscriptions a month');
    expect(area(page, 'People and money').get('.overview-area-status').text()).toBe('Billing off');
    expect(figure(page, 'Operations', 'Alarms firing')).toContain('Not read on the local stack');
  });

  it('shows the hold on hosted server limits as a banner', async () => {
    billing.mockResolvedValue({
      ok: true,
      value: adminBilling({ limits_hold: true, limits_hold_reason: 'would stop 9 of 12 owners', limits_hold_owners: 9, limits_hold_servers: 11 }),
    });
    const page = await mountOverview();
    const hold = page.get('.limits-hold');
    expect(hold.text()).toContain('Hosted server limits are on hold');
    expect(hold.text()).toContain('A pass would stop or move 11 servers of 9 owners');
    expect(hold.get('a').attributes('href')).toBe('/admin/billing');
    expect(area(page, 'People and money').get('.overview-area-status').text()).toBe('Server limits on hold');
  });

  it('reads everything again on Refresh, what needs you with it', async () => {
    const page = await mountOverview();
    expect(fleet).toHaveBeenCalledTimes(1);
    const before = needs.mock.calls.length;
    await page.findAll('button').find((b) => b.text() === 'Refresh')!.trigger('click');
    await flushPromises();
    expect(fleet).toHaveBeenCalledTimes(2);
    expect(aws).toHaveBeenCalledTimes(2);
    expect(needs.mock.calls.length).toBeGreaterThan(before);
    // The prices are read once.
    expect(tiers).toHaveBeenCalledTimes(1);
  });
});
