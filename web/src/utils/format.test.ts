import { describe, expect, it } from 'vitest';
import { formatAge, formatBitRate, formatByteRate, formatCpus, formatFrameTime, formatMemory, formatPercent } from './format';

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

describe('formatByteRate', () => {
  it('reads bytes a second in decimal units', () => {
    expect(formatByteRate(0)).toBe('0 B/s');
    expect(formatByteRate(850_000)).toBe('850 kB/s');
    expect(formatByteRate(1_234_567)).toBe('1.2 MB/s');
    expect(formatByteRate(-5)).toBe('unknown');
  });
});

describe('formatPercent', () => {
  it('reads a percentage with a space before the sign, one decimal under ten', () => {
    expect(formatPercent(41.2)).toBe('41 %');
    expect(formatPercent(4.54)).toBe('4.5 %');
    expect(formatPercent(0)).toBe('0 %');
    expect(formatPercent(180)).toBe('180 %');
    expect(formatPercent(Number.NaN)).toBe('unknown');
  });
});

describe('formatCpus', () => {
  it('reads a CPU limit in vCPU', () => {
    expect(formatCpus(0.5)).toBe('0.5 vCPU');
    expect(formatCpus(0.25)).toBe('0.25 vCPU');
    expect(formatCpus(2)).toBe('2 vCPU');
    expect(formatCpus(0)).toBe('unknown');
  });
});

describe('formatFrameTime', () => {
  it('reads a frame time in milliseconds, one decimal under a hundred', () => {
    expect(formatFrameTime(16.04)).toBe('16 ms');
    expect(formatFrameTime(16.44)).toBe('16.4 ms');
    expect(formatFrameTime(120.4)).toBe('120 ms');
    expect(formatFrameTime(-1)).toBe('unknown');
  });
});

describe('formatAge', () => {
  const now = Date.UTC(2026, 9, 18, 12, 0, 0);
  it('counts the seconds under a minute, then reads as a relative time', () => {
    expect(formatAge('2026-10-18 11:59:56', now)).toBe('4 s ago');
    expect(formatAge('2026-10-18T11:59:10Z', now)).toBe('50 s ago');
    expect(formatAge('2026-10-18T11:57:00Z', now)).toBe('3 min ago');
    expect(formatAge('2026-10-18T12:00:05Z', now)).toBe('just now');
    expect(formatAge('soon', now)).toBe('soon');
  });
});
