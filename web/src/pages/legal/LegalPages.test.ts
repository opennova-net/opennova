import { mount } from '@vue/test-utils';
import { describe, expect, it } from 'vitest';
import PrivacyPage from './PrivacyPage.vue';
import RefundsPage from './RefundsPage.vue';
import TermsPage from './TermsPage.vue';
import SiteFooter from '../../components/SiteFooter.vue';
import { testRouter } from '../../test/router';

async function mountAt(component: object, path: string) {
  const router = await testRouter(path);
  return mount(component, { global: { plugins: [router] } });
}

const DRAFT = 'Draft: the operator has not published these terms yet.';

describe('the legal pages', () => {
  it('the terms carry the draft banner and every section the operator must write', async () => {
    const wrapper = await mountAt(TermsPage, '/terms');
    expect(wrapper.get('[role="note"]').text()).toContain(DRAFT);
    expect(wrapper.findAll('h2').map((h) => h.text())).toEqual([
      'Who runs NovaWorld',
      'What a support tier is',
      'Price, currency, taxes and renewal',
      'Changing and canceling a tier',
      'No uptime guarantee',
      'Acceptable use',
      'Governing law and changes',
    ]);
    const text = wrapper.text();
    expect(text).toContain('not the sale of any game');
    expect(text).toContain('not endorsed by THQ Nordic');
    expect(text).toContain('renews automatically every month');
    expect(text).toContain("Stripe's billing portal");
    expect(text).toContain('pre-1.0 and experimental');
    expect(text).toContain('admins may stop any hosted server');
    expect(text).not.toContain('—');
    expect(text).not.toMatch(/production-ready/i);
    // The page links its siblings, marking itself.
    expect(wrapper.get('nav [aria-current="page"]').text()).toBe('Terms of service');
  });

  it('the privacy notice names the data and the processors', async () => {
    const wrapper = await mountAt(PrivacyPage, '/privacy');
    expect(wrapper.get('[role="note"]').text()).toContain(DRAFT);
    const text = wrapper.text();
    for (const words of ['Stripe customer id', 'email address go to Stripe', 'IP addresses', 'stats kept as counts', 'Amazon Web Services', 'Cloudflare', 'How long it is kept']) {
      expect(text).toContain(words);
    }
    expect(text).not.toContain('—');
  });

  it('the refund policy carries the draft banner', async () => {
    const wrapper = await mountAt(RefundsPage, '/refunds');
    expect(wrapper.get('[role="note"]').text()).toContain(DRAFT);
    expect(wrapper.findAll('h2').map((h) => h.text())).toEqual(['Refunds', 'Canceling', 'How to ask']);
  });

  it('the footer links the support tiers and the three legal pages', async () => {
    const wrapper = await mountAt(SiteFooter, '/');
    const links = Object.fromEntries(wrapper.findAll('a').map((a) => [a.text(), a.attributes('href')]));
    expect(links['Support tiers']).toBe('/support');
    expect(links['Terms']).toBe('/terms');
    expect(links['Privacy']).toBe('/privacy');
    expect(links['Refunds']).toBe('/refunds');
  });
});
