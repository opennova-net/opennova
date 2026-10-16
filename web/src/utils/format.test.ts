import { describe, expect, it } from 'vitest';
import { formatBitRate, formatMemory } from './format';

describe('formatMemory', () => {
  it('reads memory in binary units, one decimal under ten', () => {
    expect(formatMemory(0)).toBe('0 B');
    expect(formatMemory(512 * 1024 * 1024)).toBe('512 MiB');
    expect(formatMemory(1.5 * 1024 * 1024 * 1024)).toBe('1.5 GiB');
    expect(formatMemory(2048)).toBe('2 KiB');
    expect(formatMemory(-1)).toBe('unknown');
  });
});

describe('formatBitRate', () => {
  it('reads bytes a second as bits a second in decimal units', () => {
    expect(formatBitRate(0)).toBe('0 b/s');
    expect(formatBitRate(100)).toBe('800 b/s');
    expect(formatBitRate(106_250)).toBe('850 kb/s');
    expect(formatBitRate(150_000)).toBe('1.2 Mb/s');
    expect(formatBitRate(250_000_000)).toBe('2 Gb/s');
    expect(formatBitRate(Number.NaN)).toBe('unknown');
  });
});
