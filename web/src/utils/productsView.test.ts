import { describe, expect, it } from 'vitest';
import { findProduct, kindLine, listProducts, productLink, publishedLabel, releasesOf, releasesWorking, updaterProblems } from './productsView';
import { DFX2_PRODUCT, ESCALATION_PRODUCT, JO_PRODUCT, RUNTIME_PRODUCT, productsResponse, release, runtimeRelease } from '../test/productFixtures';

const ALL = [JO_PRODUCT, ESCALATION_PRODUCT, DFX2_PRODUCT, RUNTIME_PRODUCT];

describe('the products view', () => {
  it("addresses a product by its project key, or by its id when it has none", () => {
    expect(productLink(JO_PRODUCT)).toBe('/admin/products/JO');
    expect(productLink(RUNTIME_PRODUCT)).toBe('/admin/products/9');
    expect(findProduct(ALL, 'JOX01')).toBe(ESCALATION_PRODUCT);
    expect(findProduct(ALL, '9')).toBe(RUNTIME_PRODUCT);
    expect(findProduct(ALL, 'jo')).toBeNull();
    expect(findProduct(ALL, '99')).toBeNull();
  });

  it('says what kind a product is', () => {
    const lobbies = productsResponse().games;
    expect(kindLine(JO_PRODUCT, ALL, lobbies)).toBe('Game Joint Operations, lobby Joint Operations');
    expect(kindLine(ESCALATION_PRODUCT, ALL, lobbies)).toBe('Expansion jox01 of Joint Operations');
    expect(kindLine(RUNTIME_PRODUCT, ALL, lobbies)).toBe('App: the OpenNova runtime, built by CI');
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

  it('lists the runtime from its releases when the products leave it out', () => {
    const list = [runtimeRelease(7), release(9)];
    const products = listProducts([JO_PRODUCT], list);
    expect(products.map((p) => [p.id, p.display_name, p.kind, p.key])).toEqual([
      [1, 'Joint Operations', 'game', 'JO'],
      [9, 'OpenNova runtime', 'app', null],
    ]);
    expect(products[1].published_release?.id).toBe(7);
    expect(listProducts(ALL, list)).toHaveLength(4);
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
});
