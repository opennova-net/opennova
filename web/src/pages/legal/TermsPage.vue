<template>
  <LegalLayout id="terms" title="Terms of service">
    <LegalSection
      id="terms-operator"
      heading="Who runs NovaWorld"
      :covers="[
        'The operator\'s legal name, and whether it is a person or a company.',
        'A contact address for questions and notices (email, and a postal address where the law asks for one).',
        'The country the operator is based in.',
      ]"
    />
    <LegalSection
      id="terms-what-a-tier-buys"
      heading="What a support tier is"
      :covers="[
        'A support tier is a monthly contribution to the OpenNova project. It lets the account run hosted servers on NovaWorld (Tier 1 one at a time, Tier 2 two, Tier 3 three), and the dedicated machine add-on runs one of them on a machine of its own.',
        'It is support for the project, not the sale of any game: it includes no game, licence or game content, and playing a retail game still needs a purchased copy.',
        'OpenNova is a fan-run preservation project, not endorsed by THQ Nordic; the original games and their rights remain with their owners.',
      ]"
    />
    <LegalSection
      id="terms-price"
      heading="Price, currency, taxes and renewal"
      :covers="[
        'The price of each tier and of the add-on, and the currency (shown on the support tier page from Stripe\'s prices).',
        'Whether prices include taxes such as VAT, and how taxes are charged.',
        'That a tier renews automatically every month until it is canceled, and when each payment is taken.',
        'What happens when a payment fails: running servers keep running while Stripe retries the card, new servers cannot start, and the tier ends when Stripe gives up.',
      ]"
    />
    <LegalSection
      id="terms-cancellation"
      heading="Changing and canceling a tier"
      :covers="[
        'Changes and cancellations are made in Stripe\'s billing portal, from the support tier page.',
        'A cancellation takes effect at the end of the period already paid for; the hosted servers keep running until then and stop when it ends.',
        'Moving to a smaller tier stops the newest hosted servers beyond its number when the change takes effect.',
        'Refunds are covered by the refund policy.',
      ]"
    />
    <LegalSection
      id="terms-no-guarantee"
      heading="No uptime guarantee"
      :covers="[
        'NovaWorld and hosted servers are pre-1.0 and experimental: they may stop, restart, lose their state or be unavailable, and no uptime or performance is promised.',
        'What the operator owes, and does not, when a hosted server or NovaWorld is down.',
      ]"
    />
    <LegalSection
      id="terms-acceptable-use"
      heading="Acceptable use"
      :covers="[
        'What hosted servers and accounts may not be used for (abuse, cheating services, attacks on other services, unlawful content).',
        'That admins may stop any hosted server, suspend or ban an account, and change an account\'s entitlement, and when they do so.',
        'Whether a ban ends a support tier, and what happens to its payments.',
      ]"
    />
    <LegalSection
      id="terms-law"
      heading="Governing law and changes"
      :covers="[
        'The law that governs these terms and the courts that hear disputes, with the consumer rights that apply regardless.',
        'How the terms may change, and how subscribers are told before a change takes effect.',
      ]"
    />
  </LegalLayout>
</template>

<script setup lang="ts">
// /terms: the terms of service, a draft: the sections the operator must write
// before Stripe's live mode is switched on.
import LegalLayout from './LegalLayout.vue';
import LegalSection from './LegalSection.vue';
</script>
