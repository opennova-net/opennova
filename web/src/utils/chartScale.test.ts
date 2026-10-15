import { describe, expect, it } from 'vitest';
import { areaPath, linePath, nearestIndex, niceStep, niceTicks, scaleLinear, segments, timeTicks } from './chartScale';

describe('nice ticks', () => {
  it('steps by 1, 2 or 5 times a power of ten', () => {
    expect(niceStep(47, 5)).toBe(10);
    expect(niceStep(7, 4)).toBe(2);
    expect(niceStep(0.9, 4)).toBe(0.2);
    expect(niceStep(13, 4)).toBe(5);
    expect(niceStep(3_200_000, 4)).toBe(1_000_000);
    expect(niceStep(0, 4)).toBe(1);
  });

  it('covers the values on round numbers', () => {
    expect(niceTicks(0, 47, 5)).toEqual({ domain: [0, 50], ticks: [0, 10, 20, 30, 40, 50] });
    expect(niceTicks(3, 18, 4)).toEqual({ domain: [0, 20], ticks: [0, 5, 10, 15, 20] });
    expect(niceTicks(0, 0.9, 4).ticks).toEqual([0, 0.2, 0.4, 0.6, 0.8, 1]);
  });

  it('keeps whole steps for counts, and a flat or empty span still has a domain', () => {
    expect(niceTicks(0, 3, 4, true)).toEqual({ domain: [0, 3], ticks: [0, 1, 2, 3] });
    expect(niceTicks(0, 1, 4, true).ticks).toEqual([0, 1]);
    expect(niceTicks(0, 0, 4, true)).toEqual({ domain: [0, 1], ticks: [0, 1] });
    expect(niceTicks(12, 12, 4, true).domain[1]).toBeGreaterThan(12);
    expect(niceTicks(NaN, NaN, 4).domain).toEqual([0, 1]);
  });
});

describe('scaleLinear', () => {
  it('maps the domain onto the range, flipped for an SVG y', () => {
    const y = scaleLinear([0, 50], [200, 0]);
    expect(y(0)).toBe(200);
    expect(y(25)).toBe(100);
    expect(y(50)).toBe(0);
    expect(scaleLinear([5, 5], [0, 100])(5)).toBe(50);
  });
});

describe('paths', () => {
  it('draws a line through the points', () => {
    expect(
      linePath([
        { x: 0, y: 10 },
        { x: 10, y: 20 },
        { x: 20, y: 5.555 },
      ]),
    ).toBe('M0,10L10,20L20,5.56');
  });

  it('leaves a gap where the server was not running', () => {
    const points = [
      { x: 0, y: 10 },
      { x: 10, y: 12 },
      { x: 20, y: null },
      { x: 30, y: null },
      { x: 40, y: 4 },
      { x: 50, y: 6 },
      { x: 60, y: null },
      { x: 70, y: 3 },
    ];
    expect(segments(points)).toHaveLength(3);
    expect(linePath(points)).toBe('M0,10L10,12M40,4L50,6M70,3h0');
    // The area closes each run of two or more onto the baseline, and skips a lone point.
    expect(areaPath(points, 100)).toBe('M0,100L0,10L10,12L10,100ZM40,100L40,4L50,6L50,100Z');
    expect(linePath([])).toBe('');
    expect(linePath([{ x: 0, y: null }])).toBe('');
  });
});

describe('time ticks', () => {
  it('falls on round times inside the span', () => {
    const start = Date.UTC(2026, 9, 14, 12, 7);
    const hour = timeTicks(start, start + 60 * 60_000, 5);
    expect(hour.map((t) => new Date(t).getUTCMinutes())).toEqual([15, 30, 45, 0]);
    const day = timeTicks(start, start + 24 * 3_600_000, 5);
    expect(day.every((t) => new Date(t).getUTCMinutes() === 0 && new Date(t).getUTCHours() % 6 === 0)).toBe(true);
    const month = timeTicks(start, start + 30 * 86_400_000, 5);
    expect(month.length).toBeGreaterThanOrEqual(4);
    expect(month.every((t) => new Date(t).getHours() === 0)).toBe(true);
    expect(timeTicks(start, start)).toEqual([]);
  });

  it('finds the nearest point', () => {
    expect(nearestIndex([0, 10, 20, 30], 14)).toBe(1);
    expect(nearestIndex([0, 10, 20, 30], 16)).toBe(2);
    expect(nearestIndex([0, 10, 20, 30], -5)).toBe(0);
    expect(nearestIndex([0, 10, 20, 30], 99)).toBe(3);
    expect(nearestIndex([], 3)).toBe(-1);
  });
});
