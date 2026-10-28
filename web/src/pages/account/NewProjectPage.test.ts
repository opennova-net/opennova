import { flushPromises, mount, type VueWrapper } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import type { Router } from 'vue-router';
import NewProjectPage from './NewProjectPage.vue';
import { createProject, fetchMyProjects } from '../../api/products';
import { RANGERS_PROJECT, myProjects, project } from '../../test/productFixtures';
import { testRouter } from '../../test/router';

const { refreshSession } = vi.hoisted(() => ({ refreshSession: vi.fn() }));

vi.mock('../../api/products', () => ({ createProject: vi.fn(), fetchMyProjects: vi.fn() }));
vi.mock('../../composables/useSession', async () => {
  const { shallowRef } = await import('vue');
  return { useSession: () => ({ user: shallowRef(null), refresh: refreshSession }) };
});

const mine = vi.mocked(fetchMyProjects);
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
  });

  afterEach(() => {
    wrapper?.unmount();
    wrapper = null;
  });

  it('heads the form with its title and a crumb back to the projects, and nothing else', async () => {
    const page = await mountPage();
    expect(page.get('h1').text()).toBe('New project');
    expect(page.get('nav[aria-label="Breadcrumb"] a').attributes('href')).toBe('/me/projects');
    expect(page.get('nav[aria-label="Breadcrumb"] a').text()).toBe('Projects');
    expect(page.find('.page-header p').exists()).toBe(false);
    expect(page.get('#new-project-submit').text()).toBe('Create project');
    expect(page.findAll('a').find((a) => a.text() === 'Cancel')?.attributes('href')).toBe('/me/projects');
    expect(page.findAll('label.field-label').map((l) => l.text())).toEqual(['Name', 'Key', 'A mod of', 'Short name']);
  });

  it('says why the projects could not be read, and reads them again on Try again', async () => {
    mine.mockResolvedValueOnce({ ok: false, error: { code: 'network', status: null, field: '', message: 'Could not reach the server.' } });
    const page = await mountPage();
    expect(page.get('[role="alert"]').text()).toBe('Could not reach the server.');
    expect(page.find('#new-project-form').exists()).toBe(false);
    await page.findAll('button').find((b) => b.text() === 'Try again')!.trigger('click');
    await flushPromises();
    expect(page.get('[role="alert"]').text()).toBe('');
    expect(page.find('#new-project-form').exists()).toBe(true);
  });

  it('sends an ended session to the login', async () => {
    mine.mockResolvedValueOnce({ ok: false, error: { code: 'unauthorized', status: 401, field: '', message: 'ended' } });
    await mountPage();
    expect(refreshSession).toHaveBeenCalled();
    expect(router.currentRoute.value.fullPath).toBe('/login?redirect=/me/projects/new');
  });

  it('makes a mod of a game, its key and short name following its name until typed over', async () => {
    create.mockResolvedValue({ ok: true, value: project('NIGHTOPS2', { kind: 'expansion', base: 'JO', expansion_code: 'nops' }) });
    const page = await mountPage();
    // The games the service lists, private retail ones among them, the first picked.
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
      visibility: 'public',
      base: 'JO',
      expansion_code: 'nops',
    });
    expect(router.currentRoute.value.path).toBe('/me/projects/NIGHTOPS2');
  });

  it('makes a private game for a retail game, public being the default', async () => {
    create.mockResolvedValue({ ok: true, value: RANGERS_PROJECT });
    const page = await mountPage();
    expect((page.get('input[name="new-project-visibility"][value="public"]').element as HTMLInputElement).checked).toBe(true);
    await page.get('input[name="new-project-kind"][value="game"]').setValue(true);
    expect(page.find('#new-project-base').exists()).toBe(false);
    expect(page.findAll('#new-project-game option').map((o) => o.text())).toEqual(['Joint Operations', 'Delta Force Xtreme 2']);
    await page.get('#new-project-game').setValue('dfx2');
    await page.get('input[name="new-project-visibility"][value="private"]').setValue(true);
    await page.get('#new-project-name').setValue('Rangers');
    submit(page);
    await flushPromises();
    expect(create).toHaveBeenCalledWith({ key: 'RANGERS', display_name: 'Rangers', kind: 'game', visibility: 'private', game: 'dfx2' });
  });

  it("checks the routes' rules before sending, each under its field", async () => {
    const page = await mountPage();
    submit(page);
    await flushPromises();
    expect(create).not.toHaveBeenCalled();
    expect(page.get('#new-project-name-error').text()).toBe('Enter a name.');
    expect(page.get('#new-project-key-hint').text()).toBe('A key is 1 to 11 of A-Z, 0-9 and _, without "QQ".');
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
      error: { code: 'duplicate_expansion', status: 409, field: 'expansion_code', message: 'Another mod already uses that short name.' },
    });
    submit(page);
    await flushPromises();
    expect(page.get('#new-project-short-name-hint').text()).toBe('Another mod already uses that short name.');
    expect(page.get('#new-project-key-hint').text()).toBe('A-Z, 0-9 and _; it cannot change later.');

    create.mockResolvedValueOnce({ ok: false, error: { code: 'rate_limited', status: 429, field: '', message: 'Too many requests. Try again in a little while.' } });
    submit(page);
    await flushPromises();
    expect(page.get('[role="alert"]').text()).toBe('Too many requests. Try again in a little while.');
    expect(page.find('#new-project-form').exists()).toBe(true);
  });

  it('takes the form away once the account has the most projects it may', async () => {
    const page = await mountPage();
    await page.get('#new-project-name').setValue('Night Ops');
    mine.mockResolvedValue({ ok: true, value: myProjects({ usage: { projects: 10, bytes: 0, release_bytes: 0 } }) });
    create.mockResolvedValueOnce({
      ok: false,
      error: { code: 'too_many_projects', status: 403, field: '', message: 'You have as many projects as an account may have. Delete one to make another.' },
    });
    submit(page);
    await flushPromises();
    expect(page.get('[role="alert"]').text()).toBe('You have as many projects as an account may have. Delete one to make another.');
    expect(page.find('#new-project-form').exists()).toBe(false);
    expect(page.get('.new-project-blocked').text()).toBe('You have 10 projects, the most an account may have.');
  });

  it("names a title the page does not know by its code", async () => {
    mine.mockResolvedValue({ ok: true, value: myProjects({ titles: ['jo', 'bhd'] }) });
    const page = await mountPage();
    await page.get('input[name="new-project-kind"][value="game"]').setValue(true);
    expect(page.findAll('#new-project-game option').map((o) => o.text())).toEqual(['Joint Operations', 'BHD']);
  });

  it('says there is nothing to pick when the service lists no game, and the submit says why', async () => {
    mine.mockResolvedValue({ ok: true, value: myProjects({ games: [], titles: [] }) });
    const page = await mountPage();
    expect(page.findAll('#new-project-base option').map((o) => o.text())).toEqual(['Nothing to pick']);
    await page.get('#new-project-name').setValue('Night Ops');
    submit(page);
    await flushPromises();
    expect(create).not.toHaveBeenCalled();
    expect(page.text()).toContain('NovaWorld lists no game to make a mod of right now.');

    await page.get('input[name="new-project-kind"][value="game"]').setValue(true);
    expect(page.findAll('#new-project-game option').map((o) => o.text())).toEqual(['Nothing to pick']);
    submit(page);
    await flushPromises();
    expect(create).not.toHaveBeenCalled();
    expect(page.text()).toContain('NovaWorld lists no game to make one for right now.');
    expect(page.text()).not.toContain('—');
  });

  it('keeps a key to 11 characters with no "QQ", the name suggesting one that fits', async () => {
    const page = await mountPage();
    expect(page.get('#new-project-key').attributes('maxlength')).toBe('11');
    expect(page.get('#new-project-key-hint').text()).toBe('A-Z, 0-9 and _; it cannot change later.');
    await page.get('#new-project-name').setValue('Iraqq Operations Extended');
    expect(value(page, '#new-project-key')).toBe('IRAQOPERATI');

    for (const key of ['IRAQQOPS', 'TWELVECHARSX']) {
      // The field's maxlength stops a typed twelfth character; a pasted one still reaches the check.
      await page.get('#new-project-key').setValue(key);
      submit(page);
      await flushPromises();
      expect(create).not.toHaveBeenCalled();
      expect(page.get('#new-project-key-hint').text()).toBe('A key is 1 to 11 of A-Z, 0-9 and _, without "QQ".');
    }
  });

  it('says a short name another mod uses under the short name, whatever code the conflict carries', async () => {
    const page = await mountPage();
    expect(page.get('#new-project-short-name-hint').text()).toBe('Lowercase letters and digits.');
    await page.get('#new-project-name').setValue('Night Ops');
    create.mockResolvedValueOnce({
      ok: false,
      error: { code: 'unknown', status: 409, field: 'expansion_code', message: 'Another mod already uses that short name.' },
    });
    submit(page);
    await flushPromises();
    expect(page.get('#new-project-short-name').attributes('aria-invalid')).toBe('true');
    expect(page.get('#new-project-short-name-hint').text()).toBe('Another mod already uses that short name.');
    expect(page.get('[role="alert"]').text()).toBe('');
  });
});
