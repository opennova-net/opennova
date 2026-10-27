import { flushPromises, mount } from '@vue/test-utils';
import { describe, expect, it } from 'vitest';
import PlayPage from './PlayPage.vue';
import { testRouter } from '../test/router';
async function open(path = '/play') {
  const router = await testRouter(path);
  const page = mount(PlayPage, { global: { plugins: [router] } });
  return { page, router };
}
describe('PlayPage', () => {
  it('starts with the own-game setup and its current multiplayer limits', async () => {
    const { page } = await open();
    expect(page.get('#setup-title').text()).toBe('OP: Acid Gambit');
    expect(page.text()).toContain('in co-op over LAN, or online through our NovaWorld.');
    expect(page.get('a[href="/downloads#opennova"]').text()).toBe('Download OpenNova');
    expect(page.text()).not.toMatch(/production-ready|—/);
  });
  it('loads a shared game choice and updates the URL when a different setup is chosen', async () => {
    const { page, router } = await open('/play?game=jointops');
    expect(page.get('#setup-title').text()).toBe('Joint Operations');
    expect(page.text()).toContain('your own purchased copy');
    expect(page.text()).toContain('ships no files from the original game');
    expect(page.find('a[href="/downloads#jointops"]').exists()).toBe(true);
    await page.get('input[value="dfx2"]').setValue();
    await flushPromises();
    expect(router.currentRoute.value.query.game).toBe('dfx2');
    expect(page.get('#setup-title').text()).toBe('Delta Force Xtreme 2');
    expect(page.text()).toContain('Its first launch downloads Delta Force Xtreme 2.');
    expect(page.text()).toContain('open NovaWorld in the multiplayer menu and choose OpenNova.');
    expect(page.find('a[href="/downloads#dfx2"]').exists()).toBe(true);
    expect(page.text()).not.toMatch(/in place of .*dfx2\.exe/);
    await router.push('/play?game=unknown');
    expect(page.get('#setup-title').text()).toBe('OP: Acid Gambit');
  });
  it('distinguishes the services and how each game reaches them', async () => {
    const { page } = await open();
    expect(page.text()).toContain('An account on one does not work on the other.');
    expect(page.text()).toContain('points gs.novaworld.net at the address of nw.opennova.net');
    expect(page.get('a[href$="DEVELOPING.md#test-with-retail-joint-operations"]').text()).toBe('how to add the line');
    expect(page.text()).toContain('choose OpenNova, or Original NovaWorld');
    expect(page.find('a[href="/register"]').exists()).toBe(true);
    expect(page.find('a[href="/lobby"]').exists()).toBe(true);
    expect(page.text()).not.toMatch(/in place of .*dfx2\.exe/);
  });
});
