// A polled page's refresh without the view jumping. When rows above the view
// change height, the content in view moves while the scroll position stays,
// and the browser's scroll anchoring did not hold it on the infrastructure
// page (measured: a refresh that dropped one target row moved the view 61 px).
// So a refresh notes the first element that starts in view, applies the
// change, and scrolls by however far that element moved. Vue patches keyed
// rows in place, so the element is the same one afterwards; one the change
// removed holds nothing, and at the top of the page there is nothing to hold.

import { nextTick } from 'vue';

const CANDIDATES = 'h2, h3, li, tr, article';

export async function keepInView(apply: () => void, root: ParentNode | null): Promise<void> {
  const anchor = root && window.scrollY > 0 ? firstInView(root) : null;
  const before = anchor?.getBoundingClientRect().top ?? 0;
  apply();
  await nextTick();
  if (!anchor || !anchor.isConnected) return;
  const moved = anchor.getBoundingClientRect().top - before;
  if (moved !== 0) window.scrollBy(0, moved);
}

function firstInView(root: ParentNode): Element | null {
  for (const el of root.querySelectorAll(CANDIDATES)) {
    const top = el.getBoundingClientRect().top;
    if (top >= 0 && top < window.innerHeight) return el;
  }
  return null;
}
