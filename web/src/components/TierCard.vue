<template>
  <article
    :id="`${baseId}-tier-${offer.tier}`"
    class="card flex h-full flex-col gap-3 p-5"
    :class="current ? 'border-accent ring-1 ring-accent' : ''"
    :aria-labelledby="`${baseId}-tier-${offer.tier}-heading`"
    :aria-current="current ? 'true' : undefined"
  >
    <div class="flex items-baseline justify-between gap-2">
      <h3 :id="`${baseId}-tier-${offer.tier}-heading`" class="section-heading">Tier {{ offer.tier }}</h3>
      <span v-if="current" class="badge badge-info">Your tier</span>
    </div>
    <p class="text-ink">
      <template v-if="offer.amount !== null">
        <span class="text-3xl font-semibold">{{ formatMoney(offer.amount, currency) }}</span>
        <span class="text-sm text-ink-muted"> a month</span>
      </template>
      <span v-else class="text-sm text-ink-muted">No price yet</span>
    </p>
    <p class="text-sm text-ink">
      Up to <span class="font-semibold">{{ offer.max_servers }}</span>
      hosted server{{ offer.max_servers === 1 ? '' : 's' }} at once
    </p>
    <p v-if="note" :id="`${baseId}-tier-${offer.tier}-note`" class="text-sm text-warn">{{ note }}</p>
    <p v-if="soldOut" :id="`${baseId}-tier-${offer.tier}-full`" class="text-sm text-warn">{{ HOSTING_FULL }}</p>
    <div class="mt-auto">
      <button
        type="button"
        :disabled="actionDisabled || offer.amount === null || soldOut"
        :aria-describedby="describedBy"
        class="w-full"
        :class="current ? 'btn-secondary' : 'btn-primary'"
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
// (aria-current). A note under it warns what a change to it would stop. A tier
// the fleet is sold out of (`available` false) cannot be picked and says so;
// the account's own tier keeps its Manage.
import { computed } from 'vue';
import type { TierNumber, TierOffer } from '../types/billing';
import { HOSTING_FULL } from '../utils/billingView';
import { formatMoney } from '../utils/format';

const props = defineProps<{
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

const soldOut = computed(() => !props.offer.available && !props.current);
const describedBy = computed(() => {
  const ids: string[] = [];
  if (props.note) ids.push(`${props.baseId}-tier-${props.offer.tier}-note`);
  if (soldOut.value) ids.push(`${props.baseId}-tier-${props.offer.tier}-full`);
  return ids.length > 0 ? ids.join(' ') : undefined;
});
</script>
