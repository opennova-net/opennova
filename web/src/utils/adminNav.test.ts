import { describe, expect, it } from 'vitest';
import { ADMIN_SECTIONS, BILLING_TABS, PLAYERS_TABS, SERVERS_TABS, SYSTEM_TABS, sectionOf } from './adminNav';
import router from '../router';
import NotFoundPage from '../pages/NotFoundPage.vue';

describe('the admin map', () => {
  it('has six sections', () => {
    expect(ADMIN_SECTIONS.map((s) => s.label)).toEqual(['Overview', 'Players', 'Servers', 'Products', 'Billing', 'System']);
  });

  it('names pages the router opens', () => {
    const paths = [...ADMIN_SECTIONS, ...PLAYERS_TABS, ...SERVERS_TABS, ...BILLING_TABS, ...SYSTEM_TABS].map((s) => String(s.to));
    for (const path of paths) {
      expect(router.resolve(path).matched.at(-1)?.components?.default, path).not.toBe(NotFoundPage);
    }
  });

  it('places a page and the pages under it, and no page by a shared prefix alone', () => {
    expect(sectionOf('/admin')?.label).toBe('Overview');
    expect(sectionOf('/admin/players/7')?.label).toBe('Players');
    expect(sectionOf('/admin/servers/12/console')?.label).toBe('Servers');
    expect(sectionOf('/admin/machines/3')?.label).toBe('Servers');
    expect(sectionOf('/admin/jobs/77')?.label).toBe('System');
    expect(sectionOf('/admin/programs/lobby')?.label).toBe('System');
    expect(sectionOf('/admin/billing/costs?x=1')?.label).toBe('Billing');
    expect(sectionOf('/admin/playersx')).toBeNull();
    expect(sectionOf('/admin/nowhere')).toBeNull();
  });
});
