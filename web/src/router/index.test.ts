import { describe, expect, it } from 'vitest';
import router from './index';
import AddressesPage from '../pages/account/AddressesPage.vue';
import AddressesSection from '../pages/admin/AddressesSection.vue';
import LobbyServerPage from '../pages/LobbyServerPage.vue';
import MachineDetailSection from '../pages/admin/MachineDetailSection.vue';
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
    ['/admin/fleet/machines/3', MachineDetailSection],
    ['/admin/fleet/12/usage', ServerUsageTab],
    ['/admin/fleet/12/logs', ServerLogsTab],
  ])('resolves %s', (path, page) => {
    const matched = router.resolve(path).matched;
    expect(matched.at(-1)?.components?.default).toBe(page);
  });

  it('sends a lobby id that is no number to the not-found page', () => {
    expect(router.resolve('/lobby/abc').matched.at(-1)?.components?.default).toBe(NotFoundPage);
  });
});
