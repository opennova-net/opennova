import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import OverviewPage from './OverviewPage.vue';
import { fetchHostedFleet, fetchServerStatus } from '../../api/admin';
import { fetchAdminBilling } from '../../api/adminBilling';
import { fetchAttention } from '../../api/attention';
import { fetchTiers } from '../../api/billing';
import { fetchCosts } from '../../api/costs';
import { fetchFleetStats } from '../../api/hostedStats';
import { fetchJobsOverview } from '../../api/jobs';
import { fetchOnline } from '../../api/online';
import type { SessionUser } from '../../api/session';
import { refreshSession } from '../../composables/useSession';
import { adminBilling, fleetResponse } from '../../test/adminFixtures';
import { tiersResponse } from '../../test/billingFixtures';
import { costs, localCosts } from '../../test/costFixtures';
import { overview } from '../../test/jobsFixtures';
import { onlinePlayer, onlineResponse } from '../../test/onlineFixtures';
import { serverStats } from '../../test/statsFixtures';
import { attention, finding } from '../../test/usageFixtures';
import { testRouter } from '../../test/router';

const BIGGY: SessionUser = { id: 1, username: 'biggy', role: 'admin' };
// The session /api/me answers: biggy, who sees billing, unless a test says.
const session = vi.hoisted(() => ({ user: null as SessionUser | null }));

vi.mock('../../api/session', async (original) => ({
  ...(await original<typeof import('../../api/session')>()),
  me: vi.fn(async () => ({ ok: true, value: session.user })),
}));

vi.mock('../../api/admin', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../../api/admin')>()),
  fetchHostedFleet: vi.fn(),
  fetchServerStatus: vi.fn(),
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
vi.mock('../../api/online', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../../api/online')>()),
  fetchOnline: vi.fn(),
}));

const fleet = vi.mocked(fetchHostedFleet);
const status = vi.mocked(fetchServerStatus);
const online = vi.mocked(fetchOnline);
const billing = vi.mocked(fetchAdminBilling);
const needs = vi.mocked(fetchAttention);
const tiers = vi.mocked(fetchTiers);
const aws = vi.mocked(fetchCosts);
const stats = vi.mocked(fetchFleetStats);
const jobs = vi.mocked(fetchJobsOverview);

type Page = ReturnType<typeof mount>;

// A tile's text (label, value, detail) and where it leads.
function tile(page: Page, label: string) {
  const t = page.findAll('.stat-tile').find((x) => x.get('dt').text() === label)!;
  return { text: t.text(), href: t.get('a').attributes('href'), classes: t.classes() };
}

describe('OverviewPage', () => {
  let wrapper: Page | null = null;

  async function mountOverview() {
    const router = await testRouter('/admin');
    wrapper = mount(OverviewPage, { global: { plugins: [router] } });
    await flushPromises();
    return wrapper;
  }

  beforeEach(async () => {
    vi.clearAllMocks();
    session.user = BIGGY;
    await refreshSession();
    fleet.mockResolvedValue({ ok: true, value: fleetResponse() });
    status.mockResolvedValue({ ok: true, value: { maintenance_enabled: false, message: '' } });
    online.mockResolvedValue({ ok: true, value: onlineResponse([onlinePlayer('Ghost'), onlinePlayer('Viper', { state: 'lobby', server: null })]) });
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

  it('is a title, a tile per section and what needs you, with no prose', async () => {
    const page = await mountOverview();
    expect(page.get('h1').text()).toBe('Overview');
    expect(page.find('nav[aria-label="Breadcrumb"]').exists()).toBe(false);
    expect(page.findAll('.stat-tile dt').map((d) => d.text())).toEqual([
      'Players online',
      'Servers live',
      'Machines',
      'NovaWorld sign-in',
      'Subscribers',
      'AWS this month',
      'Jobs failed today',
      'Releases to approve',
    ]);
    expect(page.text()).toContain('Needs you');
  });

  it('opens the page that holds what each tile counts', async () => {
    const page = await mountOverview();
    expect(tile(page, 'Players online').href).toBe('/admin/players');
    expect(tile(page, 'Servers live').href).toBe('/admin/servers');
    expect(tile(page, 'Machines').href).toBe('/admin/servers/machines');
    expect(tile(page, 'NovaWorld sign-in').href).toBe('/admin/system');
    expect(tile(page, 'Subscribers').href).toBe('/admin/billing');
    expect(tile(page, 'AWS this month').href).toBe('/admin/billing/costs');
    expect(tile(page, 'Jobs failed today').href).toBe('/admin/system/jobs');
    expect(tile(page, 'Releases to approve').href).toBe('/admin/products');
  });

  it('shows the key numbers', async () => {
    const page = await mountOverview();
    expect(tile(page, 'Players online').text).toContain('1 playing, 1 in the lobby');
    expect(tile(page, 'Servers live').text).toContain('5 players on them');
    expect(page.get('.stat-tile svg').attributes('aria-label')).toContain('Players on hosted servers, last 24 hours');
    expect(tile(page, 'Machines').text).toContain('1 shared, 1 dedicated');
    expect(tile(page, 'NovaWorld sign-in').text).toContain('Open');
    expect(tile(page, 'Subscribers').text).toContain('1 past due');
    expect(tile(page, 'Subscribers').classes).toContain('border-warn/40');
    expect(tile(page, 'AWS this month').text).toContain('$41.25');
    expect(tile(page, 'Jobs failed today').text).toContain('3 queued, 2 running');
  });

  it('marks maintenance and billing off', async () => {
    status.mockResolvedValue({ ok: true, value: { maintenance_enabled: true, message: 'Back soon' } });
    billing.mockResolvedValue({ ok: true, value: adminBilling({ enabled: false }) });
    aws.mockResolvedValue({ ok: true, value: localCosts() });
    const page = await mountOverview();
    expect(tile(page, 'NovaWorld sign-in').text).toContain('Maintenance');
    expect(tile(page, 'NovaWorld sign-in').classes).toContain('border-warn/40');
    expect(tile(page, 'Subscribers').text).toContain('Billing off');
    expect(tile(page, 'AWS this month').text).toContain('Not read here');
  });

  it('names a read that failed, and leaves the rest', async () => {
    aws.mockResolvedValue({ ok: false, error: { code: 'network', status: null, message: 'Could not reach the server.' } });
    jobs.mockResolvedValue({ ok: false, error: { code: 'db_error', status: 500, message: 'The server hit a database error.' } });
    const page = await mountOverview();
    expect(page.get('.overview-problems').text()).toBe(
      'Could not read the background jobs (The server hit a database error.); AWS costs (Could not reach the server.).',
    );
    expect(tile(page, 'Machines').text).toContain('1 shared, 1 dedicated');
  });

  it('shows the hold on hosted server limits', async () => {
    billing.mockResolvedValue({
      ok: true,
      value: adminBilling({ limits_hold: true, limits_hold_reason: 'would stop 9 of 12 owners', limits_hold_owners: 9, limits_hold_servers: 11 }),
    });
    const page = await mountOverview();
    const hold = page.get('.limits-hold');
    expect(hold.text()).toContain('Server limits are on hold');
    expect(hold.get('a').attributes('href')).toBe('/admin/billing');
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
    expect(tiers).toHaveBeenCalledTimes(1);
  });

  it('shows another admin no money and reads none', async () => {
    session.user = { id: 2, username: 'ghost', role: 'admin' };
    await refreshSession();
    billing.mockResolvedValue({ ok: true, value: adminBilling({ limits_hold: true, limits_hold_reason: 'would stop 9 of 12 owners', limits_hold_owners: 9, limits_hold_servers: 11 }) });
    needs.mockResolvedValue({
      ok: true,
      value: attention({ findings: [finding('warn', 'is oversold', { subject: 'Capacity', link: '/admin/billing' })], alarms: [] }),
    });
    const page = await mountOverview();
    expect(page.findAll('.stat-tile dt').map((d) => d.text())).toEqual([
      'Players online',
      'Servers live',
      'Machines',
      'NovaWorld sign-in',
      'Jobs failed today',
      'Releases to approve',
    ]);
    expect(page.find('.limits-hold').exists()).toBe(false);
    expect(page.find('.attention-finding').exists()).toBe(false);
    expect(page.find('a[href^="/admin/billing"]').exists()).toBe(false);
    expect(billing).not.toHaveBeenCalled();
    expect(aws).not.toHaveBeenCalled();
    expect(tiers).not.toHaveBeenCalled();
  });
});
