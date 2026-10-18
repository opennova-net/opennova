import { describe, expect, it } from 'vitest';
import { isEmailAddress } from './email';

describe('isEmailAddress', () => {
  it('takes what the server takes', () => {
    for (const ok of ['a@b.co', 'Player.One+jo@Example.COM', 'player@example.net', `${'x'.repeat(242)}@example.com`]) {
      expect(isEmailAddress(ok), ok).toBe(true);
    }
  });

  it('refuses what the server refuses', () => {
    for (const bad of [
      '',
      'plain',
      '@example.com',
      'a@',
      'a@localhost',
      'a@b@c.com',
      'a@.example.com',
      'a@example.com.',
      'a@example..com',
      'a b@example.com',
      'a@exa\tmple.com',
      `${'x'.repeat(243)}@example.com`,
    ]) {
      expect(isEmailAddress(bad), bad).toBe(false);
    }
  });
});
