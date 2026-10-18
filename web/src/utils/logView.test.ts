import { describe, expect, it } from 'vitest';
import { appendNewer, lineTime, logFilterProblem, logSourceText } from './logView';

const line = (at: string, text: string) => ({ at, text });

describe('appendNewer', () => {
  it("adds the page's lines past the last one shown", () => {
    const shown = [line('t1', 'a'), line('t2', 'b')];
    expect(appendNewer(shown, [line('t2', 'b'), line('t3', 'c')])).toEqual([...shown, line('t3', 'c')]);
    expect(appendNewer(shown, [line('t1', 'a'), line('t2', 'b')])).toEqual(shown);
    expect(appendNewer([], [line('t3', 'c')])).toEqual([line('t3', 'c')]);
  });

  it("is null when the page does not reach back to the last line shown", () => {
    expect(appendNewer([line('t2', 'b')], [line('t5', 'e'), line('t6', 'f')])).toBeNull();
    expect(appendNewer([line('t2', 'b')], [])).toBeNull();
  });
});

describe('the log words', () => {
  it('says where a page was read', () => {
    expect(logSourceText({ source: 'cloudwatch', group: '/opennova/prod/servers', stream: '12/g2' })).toBe(
      'CloudWatch /opennova/prod/servers, stream 12/g2',
    );
    expect(logSourceText({ source: 'cloudwatch', group: '/opennova/prod/service', stream: null })).toBe(
      'CloudWatch /opennova/prod/service, every stream of it',
    );
    expect(logSourceText({ source: 'file', group: '/var/log/opennova', stream: 'servers/12/g2.log' })).toBe(
      'The log directory /var/log/opennova, servers/12/g2.log',
    );
    expect(logSourceText({ source: 'file', group: null, stream: null })).toBe('Nothing to read yet: no log names this yet.');
  });

  it("reads a line's time to the second", () => {
    expect(lineTime('2026-10-18T14:05:03.123Z')).toBe('2026-10-18 14:05:03');
    expect(lineTime(null)).toBe('');
  });

  it('refuses a filter the route would', () => {
    expect(logFilterProblem('serving on')).toBe('');
    expect(logFilterProblem('a\\b')).toBe('The filter cannot hold a quote or a backslash.');
    expect(logFilterProblem('x'.repeat(201))).toBe('The filter is at most 200 bytes.');
  });
});
