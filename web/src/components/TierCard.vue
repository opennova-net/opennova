<template>
  <article
    :id="`${baseId}-tier-${offer.tier}`"
    class="flex h-full flex-col gap-4 rounded-panel border bg-panel p-5"
    :class="current ? 'border-accent ring-1 ring-accent' : 'border-border'"
    :aria-labelledby="`${baseId}-tier-${offer.tier}-heading`"
    :aria-current="current ? 'true' : undefined"
  >
    <div class="flex items-baseline justify-between gap-2">
      <h3 :id="`${baseId}-tier-${offer.tier}-heading`" class="text-lg font-semibold text-ink">Tier {{ offer.tier }}</h3>
      <span v-if="current" class="rounded-control bg-accent/15 px-2 py-0.5 text-xs font-semibold text-accent">Your tier</span>
    </div>
    <p class="text-ink">
      <template v-if="offer.amount !== null">
        <span class="text-3xl font-semibold">{{ formatMoney(offer.amount, currency) }}</span>
        <span class="text-sm text-ink-muted"> a month</span>
      </template>
      <span v-else class="text-sm text-ink-muted">Price not set yet</span>
    </p>
    <p class="text-sm text-ink">
      Up to <span class="font-semibold">{{ offer.max_servers }}</span>
      hosted server{{ offer.max_servers === 1 ? '' : 's' }} at once
    </p>
    <p v-if="note" :id="`${baseId}-tier-${offer.tier}-note`" class="text-sm text-warn">{{ note }}</p>
    <div class="mt-auto">
      <button
        type="button"
        :disabled="actionDisabled || offer.amount === null"
        :aria-describedby="note ? `${baseId}-tier-${offer.tier}-note` : undefined"
        class="w-full rounded-control px-4 py-2 text-sm font-semibold transition disabled:cursor-not-allowed disabled:opacity-40"
        :class="current ? 'border border-border bg-raised text-ink hover:bg-hover' : 'bg-accent text-on-accent hover:bg-accent/90'"
        @click="emit('choose', offer.tier)"
      >
        {{ actionLabel }}<span class="sr-only">: Tier {{ offer.tier }}</span>
      </button>
    </div>
  </article>
</template>

<script setup lang="ts">
// One support tier: its price a month, the hosted servers it covers and its
// one action (Subscribe, Manage, Change), the account's own tier marked
// (aria-current). A note under it warns what a change to it would stop.
import type { TierNumber, TierOffer } from '../types/billing';
import { formatMoney } from '../utils/format';

defineProps<{
  offer: TierOffer;
  currency: string;
  // The base of the card's ids.
  baseId: string;
  // The account's own tier.
  current?: boolean;
  actionLabel: string;
  actionDisabled?: boolean;
  note?: string;
}>();

const emit = defineEmits<{
  (e: 'choose', tier: TierNumber): void;
}>();
</script>
