import { describe, expect, it } from 'vitest';
import router from './index';
import AddressesPage from '../pages/account/AddressesPage.vue';
import AddressesSection from '../pages/admin/AddressesSection.vue';
import CostsSection from '../pages/admin/CostsSection.vue';
import LobbyServerPage from '../pages/LobbyServerPage.vue';
import MachineDetailSection from '../pages/admin/MachineDetailSection.vue';
import InfraResourceSection from '../pages/admin/InfraResourceSection.vue';
import ProgramDetailSection from '../pages/admin/ProgramDetailSection.vue';
import NotFoundPage from '../pages/NotFoundPage.vue';
import ServerLogsTab from '../pages/servers/ServerLogsTab.vue';
import ServerUsageTab from '../pages/servers/ServerUsageTab.vue';

// The routes whose paths take a numeric id resolve a real one: a pattern's
// \\d in a string literal must stay escaped, or it matches the letter d.
describe('router', () => {
  it.each([
    ['/lobby/167772175', LobbyServerPage],
    // The billing portal's cancel flow for an address returns here.
    ['/me/addresses', AddressesPage],
    ['/admin/addresses', AddressesSection],
    ['/admin/costs', CostsSection],
    ['/admin/fleet/machines/3', MachineDetailSection],
    ['/admin/fleet/12/usage', ServerUsageTab],
    ['/admin/fleet/12/logs', ServerLogsTab],
    ['/admin/infrastructure/programs/web', ProgramDetailSection],
    ['/admin/infrastructure/resources/nlb', InfraResourceSection],
  ])('resolves %s', (path, page) => {
    const matched = router.resolve(path).matched;
    expect(matched.at(-1)?.components?.default).toBe(page);
  });

  it('sends a lobby id that is no number to the not-found page', () => {
    expect(router.resolve('/lobby/abc').matched.at(-1)?.components?.default).toBe(NotFoundPage);
  });

  it('sends an AWS resource the view reads none of to the not-found page', () => {
    expect(router.resolve('/admin/infrastructure/resources/ec2').matched.at(-1)?.components?.default).toBe(NotFoundPage);
  });
});
