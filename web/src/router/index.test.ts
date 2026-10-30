import { describe, expect, it } from 'vitest';
import router from './index';
import PublicServerPage from '../pages/PublicServerPage.vue';
import NotFoundPage from '../pages/NotFoundPage.vue';
import PlayPage from '../pages/PlayPage.vue';
import DownloadsPage from '../pages/DownloadsPage.vue';
import HostingPage from '../pages/HostingPage.vue';
import AccountOverviewPage from '../pages/account/AccountOverviewPage.vue';
import BillingPage from '../pages/account/BillingPage.vue';
import HostedGamePage from '../pages/account/HostedGamePage.vue';
import MyServerStatsPage from '../pages/account/MyServerStatsPage.vue';
import OverviewPage from '../pages/admin/OverviewPage.vue';
import OnlineTab from '../pages/admin/players/OnlineTab.vue';
import AccountsTab from '../pages/admin/players/AccountsTab.vue';
import PlayerPage from '../pages/admin/players/PlayerPage.vue';
import CdKeysTab from '../pages/admin/players/CdKeysTab.vue';
import ServersTab from '../pages/admin/servers/ServersTab.vue';
import MachinesTab from '../pages/admin/servers/MachinesTab.vue';
import RollTab from '../pages/admin/servers/RollTab.vue';
import MachinePage from '../pages/admin/servers/MachinePage.vue';
import AddressesTab from '../pages/admin/billing/AddressesTab.vue';
import CostsTab from '../pages/admin/billing/CostsTab.vue';
import NovaWorldTab from '../pages/admin/system/NovaWorldTab.vue';
import JobsTab from '../pages/admin/system/JobsTab.vue';
import ProgramPage from '../pages/admin/system/ProgramPage.vue';
import ResourcePage from '../pages/admin/system/ResourcePage.vue';
import ServerLogsTab from '../pages/servers/ServerLogsTab.vue';

async function resolvedPage(path: string) {
  const component = router.resolve(path).matched.at(-1)?.components?.default;
  return typeof component === 'function' ? (await (component as () => Promise<{ default: unknown }>)()).default : component;
}

// Where a path lands after its redirects, as a full path.
function landing(path: string): string {
  let at = router.resolve(path);
  for (let hops = 0; hops < 5; hops++) {
    const record = at.matched.at(-1);
    if (!record?.redirect) return at.fullPath;
    const target = typeof record.redirect === 'function' ? record.redirect(at, at as never) : record.redirect;
    at = router.resolve(target as Parameters<typeof router.resolve>[0]);
  }
  return at.fullPath;
}

// The routes whose paths take a numeric id resolve a real one: a pattern's
// \\d in a string literal must stay escaped, or it matches the letter d.
describe('router', () => {
  it.each([
    ['/play', PlayPage],
    ['/downloads', DownloadsPage],
    ['/hosting', HostingPage],
    ['/me', AccountOverviewPage],
    ['/me/billing', BillingPage],
    ['/me/servers/stats', MyServerStatsPage],
    ['/me/servers/hosts/167772161', HostedGamePage],
    ['/lobby/167772175', PublicServerPage],
    ['/lobby/novalogic/167782351', PublicServerPage],
    ['/admin', OverviewPage],
    ['/admin/players', OnlineTab],
    ['/admin/players/accounts', AccountsTab],
    ['/admin/players/cd-keys', CdKeysTab],
    ['/admin/players/7', PlayerPage],
    ['/admin/servers', ServersTab],
    ['/admin/servers/machines', MachinesTab],
    ['/admin/servers/roll', RollTab],
    ['/admin/servers/12/logs', ServerLogsTab],
    ['/admin/machines/3', MachinePage],
    ['/admin/billing/addresses', AddressesTab],
    ['/admin/billing/costs', CostsTab],
    ['/admin/system', NovaWorldTab],
    ['/admin/system/jobs', JobsTab],
    ['/admin/programs/web', ProgramPage],
    ['/admin/resources/nlb', ResourcePage],
  ])('resolves %s', async (path, page) => {
    expect(await resolvedPage(path)).toBe(page);
  });

  // The account's billing page's and the public hosting page's old paths, which
  // links outside the site carry: each lands on its new place.
  it.each([
    ['/me/support?checkout=cancel', '/me/billing?checkout=cancel'],
    ['/me/addresses', '/me/billing#addresses'],
    ['/me/servers/5/settings', '/me/servers/5'],
    ['/me/account', '/me'],
    ['/support', '/hosting'],
  ])('sends %s to %s', (path, to) => {
    expect(landing(path)).toBe(to);
  });

  it('sends a lobby id that is no number to the not-found page', async () => {
    expect(await resolvedPage('/lobby/abc')).toBe(NotFoundPage);
    expect(await resolvedPage('/lobby/novalogic/abc')).toBe(NotFoundPage);
  });

  it('sends an AWS resource the view reads none of to the not-found page', async () => {
    expect(await resolvedPage('/admin/resources/ec2')).toBe(NotFoundPage);
  });
});
