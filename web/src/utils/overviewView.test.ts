import { describe, expect, it } from 'vitest';
import { areaAttention, areaOfLink, counted, needWords, worstStatus } from './overviewView';
import { attention, finding } from '../test/usageFixtures';

describe("the Overview's areas", () => {
  it('places a finding by the page it links to', () => {
    expect(areaOfLink('/admin/fleet/machines/3')).toBe('hosting');
    expect(areaOfLink('/admin/fleet?tab=roll')).toBe('hosting');
    expect(areaOfLink('/admin/addresses')).toBe('hosting');
    expect(areaOfLink('/admin/billing')).toBe('people');
    expect(areaOfLink('/admin/infrastructure/programs/lobby')).toBe('operations');
    expect(areaOfLink('/admin/novaworld#sessions')).toBe('novaworld');
    expect(areaOfLink(null)).toBeNull();
    expect(areaOfLink('https://example.com/')).toBeNull();
  });

  it("counts what needs you in an area, danger or a firing alarm the worse, never an info", () => {
    expect(areaAttention(attention(), 'hosting')).toEqual({ tone: 'bad', label: '2 need you' });
    expect(areaAttention(attention(), 'operations')).toEqual({ tone: 'bad', label: '1 needs you' });
    expect(areaAttention(attention(), 'novaworld')).toBeNull();
    expect(areaAttention(attention({ findings: [finding('warn', 'oversold', { link: '/admin/billing' })], alarms: [] }), 'people')).toEqual({
      tone: 'warn',
      label: '1 needs you',
    });
    expect(areaAttention(attention({ findings: [finding('info', 'held', { link: '/admin/fleet' })], alarms: [] }), 'hosting')).toBeNull();
    expect(areaAttention(null, 'hosting')).toBeNull();
  });

  it('takes the worst state, the first of the worst', () => {
    const fallback = { tone: 'ok' as const, label: 'Running' };
    expect(worstStatus([], fallback)).toBe(fallback);
    expect(worstStatus([null, { tone: 'warn', label: 'a' }, { tone: 'bad', label: 'b' }, { tone: 'bad', label: 'c' }], fallback)).toEqual({ tone: 'bad', label: 'b' });
    expect(needWords(1)).toBe('1 needs you');
    expect(needWords(4)).toBe('4 need you');
    expect(counted(1, 'job failed', 'jobs failed')).toBe('1 job failed');
    expect(counted(3, 'job failed', 'jobs failed')).toBe('3 jobs failed');
  });
});
