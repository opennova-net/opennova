import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { keepInView } from './keepInView';

// jsdom lays nothing out: each row's top is set by hand, as the browser would
// report it after the change applies.
function row(root: HTMLElement, top: () => number): HTMLElement {
  const li = document.createElement('li');
  li.getBoundingClientRect = () => ({ top: top() }) as DOMRect;
  root.append(li);
  return li;
}

describe('keepInView', () => {
  let root: HTMLElement;
  let scrollBy: ReturnType<typeof vi.fn>;

  beforeEach(() => {
    root = document.createElement('ul');
    document.body.append(root);
    scrollBy = vi.fn();
    window.scrollBy = scrollBy as unknown as typeof window.scrollBy;
    Object.defineProperty(window, 'scrollY', { configurable: true, value: 2400 });
    Object.defineProperty(window, 'innerHeight', { configurable: true, value: 800 });
  });

  afterEach(() => {
    root.remove();
  });

  it('scrolls by as far as the first row in view moved', async () => {
    let shrunk = false;
    row(root, () => (shrunk ? -400 : -300)); // above the view
    row(root, () => (shrunk ? 175 : 236)); // the first in view
    row(root, () => (shrunk ? 500 : 561));
    await keepInView(() => {
      shrunk = true;
    }, root);
    expect(scrollBy).toHaveBeenCalledWith(0, -61);
  });

  it('leaves the view be when nothing in it moved', async () => {
    row(root, () => 120);
    await keepInView(() => {}, root);
    expect(scrollBy).not.toHaveBeenCalled();
  });

  it('holds nothing at the top of the page, or once the row in view is gone', async () => {
    const first = row(root, () => 100);
    Object.defineProperty(window, 'scrollY', { configurable: true, value: 0 });
    let applied = false;
    await keepInView(() => {
      applied = true;
    }, root);
    expect(applied).toBe(true);
    expect(scrollBy).not.toHaveBeenCalled();

    Object.defineProperty(window, 'scrollY', { configurable: true, value: 900 });
    await keepInView(() => first.remove(), root);
    expect(scrollBy).not.toHaveBeenCalled();
  });
});
