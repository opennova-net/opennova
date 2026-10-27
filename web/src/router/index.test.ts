import { describe, expect, it } from 'vitest';
import router from './index';
import AddressesPage from '../pages/account/AddressesPage.vue';
import AddressesSection from '../pages/admin/AddressesSection.vue';
import CostsSection from '../pages/admin/CostsSection.vue';
import NovaWorldSection from '../pages/admin/NovaWorldSection.vue';
import LobbyServerPage from '../pages/LobbyServerPage.vue';
import NovaLogicServerPage from '../pages/NovaLogicServerPage.vue';
import MachineDetailSection from '../pages/admin/MachineDetailSection.vue';
import InfraResourceSection from '../pages/admin/InfraResourceSection.vue';
import ProgramDetailSection from '../pages/admin/ProgramDetailSection.vue';
import NotFoundPage from '../pages/NotFoundPage.vue';
import ServerLogsTab from '../pages/servers/ServerLogsTab.vue';
import ServerUsageTab from '../pages/servers/ServerUsageTab.vue';
import PlayPage from '../pages/PlayPage.vue';
import DownloadsPage from '../pages/DownloadsPage.vue';
import AccountOverviewPage from '../pages/account/AccountOverviewPage.vue';

async function resolvedPage(path: string) {
  const component = router.resolve(path).matched.at(-1)?.components?.default;
  return typeof component === 'function' ? (await (component as () => Promise<{ default: unknown }>)()).default : component;
}

// The routes whose paths take a numeric id resolve a real one: a pattern's
// \\d in a string literal must stay escaped, or it matches the letter d.
describe('router', () => {
  it.each([
    ['/play', PlayPage],
    ['/downloads', DownloadsPage],
    ['/me', AccountOverviewPage],
    ['/lobby/167772175', LobbyServerPage],
    ['/lobby/novalogic/167782351', NovaLogicServerPage],
    // The billing portal's cancel flow for an address returns here.
    ['/me/addresses', AddressesPage],
    ['/admin/addresses', AddressesSection],
    ['/admin/costs', CostsSection],
    ['/admin/novaworld', NovaWorldSection],
    ['/admin/fleet/machines/3', MachineDetailSection],
    ['/admin/fleet/12/usage', ServerUsageTab],
    ['/admin/fleet/12/logs', ServerLogsTab],
    ['/admin/infrastructure/programs/web', ProgramDetailSection],
    ['/admin/infrastructure/resources/nlb', InfraResourceSection],
  ])('resolves %s', async (path, page) => {
    expect(await resolvedPage(path)).toBe(page);
  });

  // The NovaWorld service's and sessions' pages became one; the older names
  // before them too.
  it.each([
    ['/admin/service', { path: '/admin/novaworld' }],
    ['/admin/server', { path: '/admin/novaworld' }],
    ['/admin/sessions', { path: '/admin/novaworld', hash: '#sessions' }],
    ['/admin/connections', { path: '/admin/novaworld', hash: '#sessions' }],
    ['/admin/hosted', { path: '/admin/fleet' }],
  ])('sends %s to its new place', (path, to) => {
    const redirect = router.resolve(path).matched.at(-1)?.redirect;
    expect(typeof redirect === 'string' ? { path: redirect } : redirect).toEqual(to);
  });

  // The paths the attention list (fleet_health.cpp), the toolbox and the docs
  // name: every one still opens a page.
  it.each([
    '/admin/overview',
    '/admin/fleet',
    '/admin/fleet?tab=roll',
    '/admin/fleet/machines/12',
    '/admin/fleet/41/logs',
    '/admin/infrastructure/programs/lobby',
    '/admin/addresses',
    '/admin/billing',
    '/admin/jobs',
    '/admin/jobs/77',
    '/admin/users/7',
    '/admin/usage',
    '/admin/products',
    '/admin/products/JO',
    '/admin/cd-keys',
  ])('keeps %s', async (path) => {
    expect(await resolvedPage(path)).not.toBe(NotFoundPage);
  });

  it('sends a lobby id that is no number to the not-found page', async () => {
    expect(await resolvedPage('/lobby/abc')).toBe(NotFoundPage);
    expect(await resolvedPage('/lobby/novalogic/abc')).toBe(NotFoundPage);
  });

  it('sends an AWS resource the view reads none of to the not-found page', async () => {
    expect(await resolvedPage('/admin/infrastructure/resources/ec2')).toBe(NotFoundPage);
  });
});
