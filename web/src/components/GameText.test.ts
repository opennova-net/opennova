import { mount } from '@vue/test-utils';
import { describe, expect, it } from 'vitest';
import GameText from './GameText.vue';

function spans(text: string) {
  return mount(GameText, { props: { text } }).findAll<HTMLElement>('span > span');
}

describe('GameText', () => {
  it("draws a name's colours and leaves its tags out", () => {
    const wrapper = mount(GameText, { props: { text: 'Joe <c00FF00>COOP/AAS</c> <-c' } });
    expect(wrapper.text()).toBe('Joe COOP/AAS <-c');
    const [plain, green] = wrapper.findAll<HTMLElement>('span > span');
    expect(plain.attributes('style') ?? '').toBe('');
    expect(green.text()).toBe('COOP/AAS <-c');
    expect(green.element.style.color).toBe('rgb(0, 255, 0)');
  });

  it('draws bold, italic and underline', () => {
    const [bold, plain] = spans('<b>Bold<-b> plain');
    expect(bold.text()).toBe('Bold');
    expect(bold.element.style.fontWeight).toBe('700');
    expect(plain.element.textContent).toBe(' plain');
    expect(plain.element.style.fontWeight).toBe('');
    const [styled] = spans('<iu>x');
    expect(styled.element.style.fontStyle).toBe('italic');
    expect(styled.element.style.textDecoration).toBe('underline');
  });

  it("dims the original colour for its half-bright, and inherits it for the original", () => {
    const [red, original, half] = spans('<cFF0000>a<CO>b<CH>c');
    expect(red.element.style.color).toBe('rgb(255, 0, 0)');
    expect(original.element.style.color).toBe('');
    expect(half.element.style.color).toBe('');
    expect(half.element.style.opacity).toBe('0.75');
  });

  it('never makes HTML of a name', () => {
    const wrapper = mount(GameText, { props: { text: '<img src=x onerror=alert(1)>a\\<script>b' } });
    expect(wrapper.find('img').exists()).toBe(false);
    expect(wrapper.find('script').exists()).toBe(false);
    expect(wrapper.text()).toBe('a<script>b');
  });

  it('draws nothing for no text', () => {
    expect(mount(GameText, { props: { text: null } }).text()).toBe('');
  });
});
