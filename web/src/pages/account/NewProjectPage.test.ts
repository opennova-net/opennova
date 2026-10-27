import { flushPromises, mount, type VueWrapper } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import type { Router } from 'vue-router';
import NewProjectPage from './NewProjectPage.vue';
import { createProject, fetchMyProjects, fetchProjects } from '../../api/products';
import { ACIDGAMBIT_PROJECT, NIGHTOPS_PROJECT, RANGERS_PROJECT, myProjects, project } from '../../test/productFixtures';
import { testRouter } from '../../test/router';

const { refreshSession } = vi.hoisted(() => ({ refreshSession: vi.fn() }));

vi.mock('../../api/products', () => ({ createProject: vi.fn(), fetchMyProjects: vi.fn(), fetchProjects: vi.fn() }));
vi.mock('../../composables/useSession', async () => {
  const { shallowRef } = await import('vue');
  return { useSession: () => ({ user: shallowRef(null), refresh: refreshSession }) };
});

const mine = vi.mocked(fetchMyProjects);
const readable = vi.mocked(fetchProjects);
const create = vi.mocked(createProject);

function submit(page: VueWrapper) {
  return page.get('#new-project-form').element.dispatchEvent(new Event('submit'));
}

function value(page: VueWrapper, selector: string): string {
  return (page.get(selector).element as HTMLInputElement | HTMLSelectElement).value;
}

describe('NewProjectPage', () => {
  let wrapper: VueWrapper | null = null;
  let router: Router;

  async function mountPage() {
    router = await testRouter('/me/projects/new');
    wrapper = mount(NewProjectPage, { global: { plugins: [router] } });
    await flushPromises();
    return wrapper;
  }

  beforeEach(() => {
    vi.clearAllMocks();
    mine.mockResolvedValue({ ok: true, value: myProjects() });
    readable.mockResolvedValue({ ok: true, value: [ACIDGAMBIT_PROJECT, NIGHTOPS_PROJECT, RANGERS_PROJECT] });
  });

  afterEach(() => {
    wrapper?.unmount();
    wrapper = null;
  });

  it('makes a mod of a game, its key and short name following its name until typed over', async () => {
    create.mockResolvedValue({ ok: true, value: project('NIGHTOPS2', { kind: 'expansion', base: 'JO', expansion_code: 'nops' }) });
    const page = await mountPage();
    // The retail games first, then the games the account may read.
    expect(page.findAll('#new-project-base option').map((o) => o.text())).toEqual(['Joint Operations', 'Delta Force Xtreme 2', 'OP: Acid Gambit', 'Rangers']);
    expect(value(page, '#new-project-base')).toBe('JO');

    await page.get('#new-project-name').setValue('Night Ops 2');
    expect(value(page, '#new-project-key')).toBe('NIGHTOPS2');
    expect(value(page, '#new-project-short-name')).toBe('nightops');
    await page.get('#new-project-short-name').setValue('NOPS');
    await page.get('#new-project-name').setValue('Night Ops: Two');
    expect(value(page, '#new-project-key')).toBe('NIGHTOPSTWO');
    expect(value(page, '#new-project-short-name')).toBe('NOPS');
    await page.get('#new-project-key').setValue('nightops2');
    await page.get('#new-project-name').setValue(' Night Ops 2 ');
    expect(value(page, '#new-project-key')).toBe('nightops2');

    submit(page);
    await flushPromises();
    expect(create).toHaveBeenCalledWith({
      key: 'NIGHTOPS2',
      display_name: 'Night Ops 2',
      kind: 'expansion',
      visibility: 'private',
      base: 'JO',
      expansion_code: 'nops',
    });
    expect(router.currentRoute.value.path).toBe('/me/projects/NIGHTOPS2');
  });

  it('makes a public game for a retail game', async () => {
    create.mockResolvedValue({ ok: true, value: RANGERS_PROJECT });
    const page = await mountPage();
    await page.get('input[name="new-project-kind"][value="game"]').setValue(true);
    expect(page.find('#new-project-base').exists()).toBe(false);
    expect(page.findAll('#new-project-game option').map((o) => o.text())).toEqual(['Joint Operations', 'Delta Force Xtreme 2']);
    await page.get('#new-project-game').setValue('dfx2');
    await page.get('input[name="new-project-visibility"][value="public"]').setValue(true);
    await page.get('#new-project-name').setValue('Rangers');
    submit(page);
    await flushPromises();
    expect(create).toHaveBeenCalledWith({ key: 'RANGERS', display_name: 'Rangers', kind: 'game', visibility: 'public', game: 'dfx2' });
  });

  it("checks the routes' rules before sending, each under its field", async () => {
    const page = await mountPage();
    submit(page);
    await flushPromises();
    expect(create).not.toHaveBeenCalled();
    expect(page.get('#new-project-name-error').text()).toBe('Enter a name.');
    expect(page.get('#new-project-key-hint').text()).toBe('A key is 1 to 16 of A-Z, 0-9 and _.');
    expect(page.get('#new-project-short-name-hint').text()).toBe('A short name is 1 to 8 lowercase letters and digits.');

    await page.get('#new-project-name').setValue('Night Ops');
    await page.get('#new-project-key').setValue('NIGHT-OPS');
    await page.get('#new-project-short-name').setValue('night-op');
    submit(page);
    await flushPromises();
    expect(create).not.toHaveBeenCalled();
    expect(page.find('#new-project-name-error').exists()).toBe(false);
    expect(page.get('#new-project-key').attributes('aria-invalid')).toBe('true');
    expect(page.get('#new-project-short-name').attributes('aria-invalid')).toBe('true');
  });

  it("says the service's refusal under the field it names, else above the form", async () => {
    const page = await mountPage();
    await page.get('#new-project-name').setValue('Night Ops');
    create.mockResolvedValueOnce({
      ok: false,
      error: { code: 'duplicate_key', status: 409, field: '', message: 'Another project already has that key. Pick another.' },
    });
    submit(page);
    await flushPromises();
    expect(page.get('#new-project-key-hint').text()).toBe('Another project already has that key. Pick another.');

    create.mockResolvedValueOnce({
      ok: false,
      error: { code: 'duplicate_expansion', status: 409, field: '', message: 'That game already has a mod with that short name. Pick another.' },
    });
    submit(page);
    await flushPromises();
    expect(page.get('#new-project-short-name-hint').text()).toBe('That game already has a mod with that short name. Pick another.');
    expect(page.get('#new-project-key-hint').text()).toContain('Up to 16 of A-Z, 0-9 and _.');

    create.mockResolvedValueOnce({ ok: false, error: { code: 'rate_limited', status: 429, field: '', message: 'Too many requests. Try again in a little while.' } });
    submit(page);
    await flushPromises();
    expect(page.get('[role="alert"]').text()).toBe('Too many requests. Try again in a little while.');
    expect(page.find('#new-project-form').exists()).toBe(true);
  });

  it('takes the form away once the account has the most projects it may', async () => {
    const page = await mountPage();
    await page.get('#new-project-name').setValue('Night Ops');
    mine.mockResolvedValue({ ok: true, value: myProjects({ usage: { projects: 10, bytes: 0 } }) });
    create.mockResolvedValueOnce({
      ok: false,
      error: { code: 'too_many_projects', status: 403, field: '', message: 'You have as many projects as an account may have. Delete one to make another.' },
    });
    submit(page);
    await flushPromises();
    expect(page.get('[role="alert"]').text()).toBe('You have as many projects as an account may have. Delete one to make another.');
    expect(page.find('#new-project-form').exists()).toBe(false);
    expect(page.text()).toContain('You have 10 projects, the most an account may have.');
  });

  it('offers a mod of the retail games when the other games could not be read', async () => {
    readable.mockResolvedValue({ ok: false, error: { code: 'network', status: null, field: '', message: 'offline' } });
    const page = await mountPage();
    expect(page.findAll('#new-project-base option').map((o) => o.text())).toEqual(['Joint Operations', 'Delta Force Xtreme 2']);
    expect(page.text()).not.toContain('—');
  });
});
