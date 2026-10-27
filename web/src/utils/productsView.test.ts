import { describe, expect, it } from 'vitest';
import {
  awaitingApproval,
  creatorLabel,
  findProduct,
  kindLine,
  ownerLabel,
  ownerLink,
  productLink,
  publishedLabel,
  releaseCreator,
  releaseStatusLabel,
  releasesOf,
  releasesWorking,
  updaterProblems,
} from './productsView';
import { DFX2_PRODUCT, ESCALATION_PRODUCT, JO_PRODUCT, RUNTIME_PRODUCT, productsResponse, release, runtimeRelease } from '../test/productFixtures';

const ALL = [JO_PRODUCT, ESCALATION_PRODUCT, DFX2_PRODUCT, RUNTIME_PRODUCT];

describe('the products view', () => {
  it("addresses a product by its project key, or by its id when it has none", () => {
    expect(productLink(JO_PRODUCT)).toBe('/admin/products/JO');
    expect(productLink(RUNTIME_PRODUCT)).toBe('/admin/products/ONRUNTIME');
    expect(findProduct(ALL, 'JOX01')).toBe(ESCALATION_PRODUCT);
    expect(findProduct(ALL, '9')).toBe(RUNTIME_PRODUCT);
    expect(findProduct(ALL, 'jo')).toBeNull();
    expect(findProduct(ALL, '99')).toBeNull();
  });

  it('says what kind a product is', () => {
    const lobbies = productsResponse().games;
    expect(kindLine(JO_PRODUCT, ALL, lobbies)).toBe('Game Joint Operations, lobby Joint Operations');
    expect(kindLine(ESCALATION_PRODUCT, ALL, lobbies)).toBe('Expansion jox01 of Joint Operations');
    expect(kindLine(RUNTIME_PRODUCT, ALL, lobbies)).toBe('App, built by CI');
  });

  it("names a product's owner, NovaWorld's own as OpenNova, and finds a player's account by its name", () => {
    expect(ownerLabel(JO_PRODUCT)).toBe('OpenNova');
    expect(ownerLabel({ ...JO_PRODUCT, owner: 'biggy' })).toBe('biggy');
    expect(ownerLink({ ...JO_PRODUCT, owner: 'biggy', owner_id: 7 })).toBe('/admin/users/7');
  });

  it("finds each product's releases, the runtime's upload not yet read among them, and their published version", () => {
    const uploading = runtimeRelease(11, { status: 'uploading', product_id: null, sections: [] });
    const list = [uploading, release(10, { status: 'draft' }), release(9), runtimeRelease(7)];
    expect(releasesOf(JO_PRODUCT, list).map((r) => r.id)).toEqual([10, 9]);
    expect(releasesOf(RUNTIME_PRODUCT, list).map((r) => r.id)).toEqual([11, 7]);
    expect(publishedLabel(JO_PRODUCT, list)).toBe('1.7.5.9');
    expect(publishedLabel(RUNTIME_PRODUCT, list)).toBe('v0.0.21');
    expect(publishedLabel(DFX2_PRODUCT, list)).toBe('');
    expect(releasesWorking(list)).toBe(true);
    expect(releasesWorking([release(9)])).toBe(false);
  });

  it("checks the updater's login and command file together", () => {
    expect(updaterProblems('', '')).toEqual({});
    expect(updaterProblems('JOTRus', 'JOPTR.CMD')).toEqual({});
    expect(updaterProblems('', 'JOPTR.CMD').updater_login).toContain('needs the updater login');
    expect(updaterProblems('JO TR', 'JOPTR.EXE')).toEqual({
      updater_login: 'An updater login is 1 to 32 printable ASCII characters without spaces.',
      command_file: 'A command file is 1 to 15 characters ending in .CMD (JOPTR.CMD).',
    });
  });

  it('names who started a release: a player or an admin by account, else the token or the data program', () => {
    expect(releaseCreator('user biggy')).toEqual({ words: 'by', account: 'biggy' });
    expect(releaseCreator('admin root')).toEqual({ words: 'by admin', account: 'root' });
    expect(releaseCreator('token')).toEqual({ words: 'by the admin token', account: null });
    expect(creatorLabel('user biggy')).toBe('by biggy');
    expect(creatorLabel('admin root')).toBe('by admin root');
    expect(creatorLabel('worker')).toBe('by the data program');
    expect(creatorLabel('someone')).toBe('someone');
  });

  it('names a release awaiting approval and picks those out', () => {
    expect(releaseStatusLabel('awaiting_approval')).toBe('Awaiting approval');
    const waiting = release(5, { status: 'awaiting_approval' });
    expect(awaitingApproval([release(6, { status: 'draft' }), waiting, release(4)])).toEqual([waiting]);
  });
});
