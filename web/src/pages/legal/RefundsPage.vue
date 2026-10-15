<template>
  <LegalLayout id="refunds" title="Refund policy">
    <LegalSection
      id="refunds-policy"
      heading="Refunds"
      :covers="[
        'Whether a month already paid is refunded, in full, in part or not at all, and in which cases (a failed hosted server, an outage, a mistaken subscription).',
        'The rights a consumer has regardless (in the EU, the right to withdraw from a contract within 14 days, and when it ends once a service has started).',
      ]"
    />
    <LegalSection
      id="refunds-cancellation"
      heading="Canceling"
      :covers="[
        'Canceling in Stripe\'s billing portal ends the tier at the end of the period already paid for; no further payment is taken.',
        'Whether canceling early refunds the rest of the month.',
      ]"
    />
    <LegalSection
      id="refunds-how"
      heading="How to ask"
      :covers="[
        'Where to send a refund request, what to include (the account\'s username), and how long an answer takes.',
        'How a refund is paid back (to the card, through Stripe) and how long it takes to arrive.',
      ]"
    />
  </LegalLayout>
</template>

<script setup lang="ts">
// /refunds: the refund policy, a draft: the sections the operator must write.
import LegalLayout from './LegalLayout.vue';
import LegalSection from './LegalSection.vue';
</script>
