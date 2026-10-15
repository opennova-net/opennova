<template>
  <ul class="grid gap-4 sm:grid-cols-3">
    <li v-for="offer in tiers" :key="offer.tier">
      <TierCard
        :offer="offer"
        :currency="currency"
        :base-id="baseId"
        :current="mode === 'manage' && offer.tier === currentTier"
        :action-label="actionLabel(offer)"
        :action-disabled="busy || disabled"
        :note="noteFor(offer)"
        @choose="emit('choose', $event)"
      />
    </li>
  </ul>
</template>

<script setup lang="ts">
// The three support tiers side by side. `mode` sets their action: subscribe
// (no tier yet, or a visitor not logged in), or manage (the account's tier
// is marked, and every action opens the billing portal, where a tier is
// changed or canceled). A tier below the account's warns which of its live
// servers a move there would stop: the service stops the newest first.
import TierCard from './TierCard.vue';
import type { TierNumber, TierOffer } from '../types/billing';

const props = withDefaults(
  defineProps<{
    tiers: TierOffer[];
    currency: string;
    mode: 'subscribe' | 'manage';
    // The account's tier, in manage mode.
    currentTier?: TierNumber | null;
    // How many of the account's live hosted servers are not already
    // stopping: the ones a lower tier's limit counts (newest stop first).
    used?: number;
    busy?: boolean;
    disabled?: boolean;
    baseId?: string;
  }>(),
  { currentTier: null, used: 0, busy: false, disabled: false, baseId: 'support' },
);

const emit = defineEmits<{
  (e: 'choose', tier: TierNumber): void;
}>();

function actionLabel(offer: TierOffer): string {
  if (props.mode === 'subscribe') return 'Subscribe';
  return offer.tier === props.currentTier ? 'Manage' : 'Change';
}

// What a move to `offer` would stop, said on lower tiers only.
function noteFor(offer: TierOffer): string | undefined {
  if (props.mode !== 'manage' || props.currentTier === null || offer.tier >= props.currentTier) return undefined;
  const over = props.used - offer.max_servers;
  if (over <= 0) return undefined;
  return over === 1
    ? 'Your newest server will be stopped when the change takes effect.'
    : `Your ${over} newest servers will be stopped when the change takes effect.`;
}
</script>
