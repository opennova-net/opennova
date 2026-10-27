import { describe, expect, it } from 'vitest';
import { ADMIN_AREAS, OVERVIEW, isUnder, placeOf } from './adminNav';
import router from '../router';
import NotFoundPage from '../pages/NotFoundPage.vue';

describe('the admin map', () => {
  it('groups every page in three areas, the way the service is run', () => {
    expect(ADMIN_AREAS.map((a) => [a.label, a.sections.map((s) => s.label)])).toEqual([
      ['Service', ['NovaWorld', 'Fleet', 'Games', 'Static addresses', 'Settings']],
      ['Accounts', ['Users', 'Subscriptions', 'CD keys']],
      ['Operations', ['Infrastructure', 'Server usage', 'AWS costs', 'Background jobs']],
    ]);
  });

  it('names pages the router opens', () => {
    for (const section of [OVERVIEW, ...ADMIN_AREAS.flatMap((a) => a.sections)]) {
      expect(router.resolve(section.to).matched.at(-1)?.components?.default).not.toBe(NotFoundPage);
    }
  });

  it('places a page and the pages under it, and no page by a shared prefix alone', () => {
    expect(placeOf('/admin/fleet/12/console')?.section.label).toBe('Fleet');
    expect(placeOf('/admin/fleet/machines/3')?.area?.label).toBe('Service');
    expect(placeOf('/admin/jobs/77')?.area?.label).toBe('Operations');
    expect(placeOf('/admin/novaworld')?.area?.label).toBe('Service');
    expect(placeOf('/admin/overview')).toEqual({ area: null, section: OVERVIEW });
    expect(placeOf('/admin/nowhere')).toBeNull();
    expect(isUnder('/admin/users', { to: '/admin/user', label: '', description: '' })).toBe(false);
  });
});
