import { describe, expect, it } from 'vitest';
import { parseGameText, stripGameText, type GameTextRun } from './gameText';

function run(text: string, style: Partial<GameTextRun> = {}): GameTextRun {
  return { bold: false, italic: false, underline: false, color: 'original', ...style, text };
}

describe('parseGameText', () => {
  it('colours from a <c> tag on; a <c> with no digits changes nothing', () => {
    expect(parseGameText('<c00FF00>COOP/AAS</c>')).toEqual([run('COOP/AAS', { color: '#00FF00' })]);
    expect(parseGameText('a<cff0000>b<-c>c')).toEqual([run('a'), run('bc', { color: '#FF0000' })]);
  });

  it('draws an unterminated tag as it is, in the colour before it', () => {
    expect(parseGameText('<c00FF00>Nice-Evil<-c')).toEqual([run('Nice-Evil<-c', { color: '#00FF00' })]);
    expect(stripGameText('<b')).toBe('<b');
  });

  it('turns styles on, and off after a -', () => {
    expect(parseGameText('<b>Bold<-b> plain')).toEqual([run('Bold', { bold: true }), run(' plain')]);
    expect(parseGameText('<bu>x<-ui>y')).toEqual([
      run('x', { bold: true, underline: true }),
      run('y', { bold: true, italic: false, underline: false }),
    ]);
    expect(parseGameText('<i>x<U>y')).toEqual([run('x', { italic: true }), run('y', { italic: true, underline: true })]);
  });

  it('restores the original colour with <CO>, and its half-bright with <CH>', () => {
    expect(parseGameText('<cFF0000>a<CO>b<ch>c')).toEqual([
      run('a', { color: '#FF0000' }),
      run('b'),
      run('c', { color: 'half' }),
    ]);
  });

  it('keeps the low 24 bits of a long value, and ignores a non-digit in it', () => {
    expect(parseGameText('<c1000000>x')).toEqual([run('x', { color: '#000000' })]);
    expect(parseGameText('<c00-FF-00>x')).toEqual([run('x', { color: '#00FF00' })]);
    // Zero changes nothing, black included.
    expect(parseGameText('<c000000>x')).toEqual([run('x')]);
  });

  it('reads the rest of a colour tag as its value', () => {
    // 'b' is a hex digit there, not bold.
    expect(parseGameText('<c0000b>x')).toEqual([run('x', { color: '#00000B' })]);
    expect(parseGameText('<bc00ff00>x')).toEqual([run('x', { bold: true, color: '#00FF00' })]);
  });

  it('consumes a tab-width tag, and ignores any other letter', () => {
    expect(stripGameText('a<T120>b<xyz>c</>d<>e')).toBe('abcde');
    expect(stripGameText('a<T12')).toBe('a<T12');
  });

  it('draws the character after a backslash as it is', () => {
    expect(stripGameText('a\\<b')).toBe('a<b');
    expect(parseGameText('a\\<b>c')).toEqual([run('a<b>c')]);
    expect(stripGameText('a\\\\b')).toBe('a\\b');
    expect(stripGameText('end\\')).toBe('end');
  });

  it('ends a tag at its line, and keeps the style on the next', () => {
    expect(parseGameText('<b\n>x')).toEqual([run('<b\n>x')]);
    expect(parseGameText('<b>x\ny')).toEqual([run('x\ny', { bold: true })]);
  });

  it('skips the bytes the game draws nothing for', () => {
    expect(stripGameText('a\u0001b\u007fc€d')).toBe('abcd');
    expect(stripGameText('a\tb')).toBe('a b');
  });

  it('reads nothing as nothing', () => {
    expect(parseGameText('')).toEqual([]);
    expect(parseGameText(null)).toEqual([]);
    expect(stripGameText('<c00FF00></c>')).toBe('');
  });
});
