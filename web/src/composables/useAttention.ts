// The attention list's one read (GET /api/admin/attention), shared under the
// admin layout: the nav's count and the Overview's and the Fleet's lists show
// the same answer. AdminLayout provides it, polled every 60 s while its gate is
// open (a closed gate sends no admin request); a page mounted without it (a
// test) reads its own. A failed read keeps the last answer (as of loadedAt)
// and sets error: while error is set, that answer may be out of date. An
// admin who does not see billing gets the answer without what opens the
// Billing section (utils/billingAccess.ts).

import { inject, provide, ref, watch, type InjectionKey, type Ref } from 'vue';
import { fetchAttention } from '../api/attention';
import type { Attention } from '../types/attention';
import { seesBilling, withoutBilling } from '../utils/billingAccess';
import { usePolling } from './usePolling';
import { useSession } from './useSession';

export const ATTENTION_POLL_MS = 60_000;

export interface AttentionState {
  attention: Ref<Attention | null>;
  // The last read's failure, '' after an answer.
  error: Ref<string>;
  // When the last answer came (ms).
  loadedAt: Ref<number | null>;
  // When the last read failed (ms), each failure anew; null after an answer.
  failedAt: Ref<number | null>;
  loading: Ref<boolean>;
  refresh: () => Promise<void>;
}

const KEY: InjectionKey<AttentionState> = Symbol('attention');

function readAttention(enabled: Ref<boolean>): AttentionState {
  const { user } = useSession();
  const attention = ref<Attention | null>(null);
  const error = ref('');
  const loadedAt = ref<number | null>(null);
  const failedAt = ref<number | null>(null);
  const poll = usePolling({
    async read(signal) {
      // A closed gate: nothing to read, and no poll until it opens.
      if (!enabled.value) return true;
      const result = await fetchAttention(signal);
      if (signal.aborted) return false;
      if (result.ok) {
        attention.value = seesBilling(user.value) ? result.value : withoutBilling(result.value);
        loadedAt.value = Date.now();
        error.value = '';
        failedAt.value = null;
        return true;
      }
      error.value = result.error.message;
      failedAt.value = Date.now();
      return false;
    },
    next: () => (enabled.value ? ATTENTION_POLL_MS : null),
    immediate: false,
  });
  watch(enabled, (on) => on && void poll.refresh(), { immediate: true });
  return { attention, error, loadedAt, failedAt, loading: poll.loading, refresh: poll.refresh };
}

// The layout's: read while `enabled` holds, and given to every page under it.
export function provideAttention(enabled: Ref<boolean>): AttentionState {
  const state = readAttention(enabled);
  provide(KEY, state);
  return state;
}

// A page's: the layout's read, or its own when it has none.
export function useAttention(): AttentionState {
  return inject(KEY, null) ?? readAttention(ref(true));
}
