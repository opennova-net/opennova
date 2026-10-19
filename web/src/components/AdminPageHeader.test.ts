import { mount } from '@vue/test-utils';
import { describe, expect, it } from 'vitest';
import AdminPageHeader from './AdminPageHeader.vue';
import { testRouter } from '../test/router';

interface HeaderProps {
  description?: string;
  loadedAt?: number | null;
  now?: number;
  loading?: boolean;
  refreshable?: boolean;
  crumbs?: boolean;
}

async function header(path: string, props: HeaderProps, slots: Record<string, string> = {}) {
  const router = await testRouter(path);
  return mount(AdminPageHeader, { props: { title: 'Fleet', ...props }, slots, global: { plugins: [router] } });
}

describe('AdminPageHeader', () => {
  it('says where the page sits, its name and what it holds', async () => {
    const page = await header('/admin/fleet', { description: 'The machines and their servers.' });
    expect(page.get('nav[aria-label="Breadcrumb"]').text()).toBe('Hosting');
    expect(page.get('h1').text()).toBe('Fleet');
    expect(page.get('header p').text()).toBe('The machines and their servers.');
  });

  it('says when the page was read, and refreshes on request, saying while it reads', async () => {
    const now = Date.now();
    const page = await header('/admin/fleet', { loadedAt: now - 120_000, now });
    expect(page.text()).toContain('Updated 2 min ago');
    await page.get('button').trigger('click');
    expect(page.emitted('refresh')).toHaveLength(1);
    await page.setProps({ loading: true });
    expect(page.get('button').text()).toBe('Refreshing…');
    expect(page.get('button').attributes('disabled')).toBeDefined();
  });

  it('takes a description with links, more lines and actions of its own', async () => {
    const page = await header(
      '/admin/novaworld',
      { refreshable: false },
      { description: 'See <a href="/admin/cd-keys">CD keys</a>.', default: '<p class="more">More</p>', actions: '<button class="own">Do it</button>' },
    );
    expect(page.get('header p a').attributes('href')).toBe('/admin/cd-keys');
    expect(page.find('.more').exists()).toBe(true);
    expect(page.findAll('button').map((b) => b.text())).toEqual(['Do it']);
  });

  it('leaves the breadcrumb out where asked', async () => {
    const page = await header('/admin/fleet', { crumbs: false });
    expect(page.find('nav').exists()).toBe(false);
  });
});
