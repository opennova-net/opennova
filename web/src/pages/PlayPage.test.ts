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
  it('starts on OP: Acid Gambit with up to three steps and its download', async () => {
    const { page } = await open();
    expect(page.get('h1').text()).toBe('Play');
    expect(page.get('#setup-title').text()).toBe('OP: Acid Gambit');
    expect(page.findAll('ol li').length).toBeLessThanOrEqual(3);
    expect(page.get('a[href="/downloads#opennova"]').text()).toBe('Download OpenNova');
    expect(page.text()).not.toMatch(/production-ready|—/);
  });

  it('loads a shared game choice and updates the URL when another is picked', async () => {
    const { page, router } = await open('/play?game=jointops');
    expect(page.get('#setup-title').text()).toBe('Joint Operations');
    expect(page.find('a[href="/downloads#jointops"]').exists()).toBe(true);
    await page.get('input[value="dfx2"]').setValue();
    await flushPromises();
    expect(router.currentRoute.value.query.game).toBe('dfx2');
    expect(page.get('#setup-title').text()).toBe('Delta Force Xtreme 2');
    expect(page.findAll('ol li').length).toBeLessThanOrEqual(3);
    expect(page.find('a[href="/downloads#dfx2"]').exists()).toBe(true);
    await router.push('/play?game=unknown');
    expect(page.get('#setup-title').text()).toBe('OP: Acid Gambit');
  });

  it('says how to play online in a few lines, and links the original-game doc', async () => {
    const { page } = await open();
    const online = page.get('#online');
    expect(online.findAll('p')).toHaveLength(3);
    expect(online.find('a[href="/register"]').exists()).toBe(true);
    expect(online.find('a[href="/lobby"]').exists()).toBe(true);
    const doc = online.get('a[href$="DEVELOPING.md#test-with-retail-joint-operations"]');
    expect(doc.element.parentElement?.textContent).toContain('Using the original Jointops.exe');
    // No hosts-file how-to on the page itself.
    expect(page.text()).not.toContain('gs.novaworld.net');
  });
});
