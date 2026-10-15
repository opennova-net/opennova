<template>
  <LegalLayout id="privacy" title="Privacy notice">
    <LegalSection
      id="privacy-controller"
      heading="Who is responsible"
      :covers="[
        'The operator\'s legal name, country and contact address for privacy questions.',
      ]"
    />
    <LegalSection
      id="privacy-data"
      heading="What NovaWorld keeps"
      :covers="[
        'The account: its username, NovaWorld handle, PCID, a hash of its password (never the password), its role and status.',
        'For a support tier: the account\'s Stripe customer id and its subscriptions\' status. Card details and the email address go to Stripe, which collects them; NovaWorld does not keep them.',
        'IP addresses, in the service\'s logs and in the NovaWorld sessions of game clients signed in.',
        'Hosted servers: their settings, their console logs, and stats kept as counts (players, minutes running, data sent), not who played.',
        'The CD keys of the games that use them, and the machines that activated them.',
      ]"
    />
    <LegalSection
      id="privacy-why"
      heading="Why, and on what basis"
      :covers="[
        'What each kind of data is used for (running accounts and hosted servers, billing, keeping the service safe), and the legal basis for each.',
      ]"
    />
    <LegalSection
      id="privacy-processors"
      heading="Who else handles it"
      :covers="[
        'Stripe: payments and the billing portal.',
        'Amazon Web Services: the servers and the database NovaWorld runs on, and where they are.',
        'Cloudflare: the site\'s network edge, which sees every visit to it.',
        'Any transfer of data outside the visitor\'s country, and the safeguards for it.',
      ]"
    />
    <LegalSection
      id="privacy-retention"
      heading="How long it is kept"
      :covers="[
        'How long accounts, logs, console logs, stats, billing records and backups are kept, and what happens when an account is deleted.',
      ]"
    />
    <LegalSection
      id="privacy-rights"
      heading="Your rights"
      :covers="[
        'How to see, correct, export or delete your data, and how to object or complain to a regulator.',
      ]"
    />
  </LegalLayout>
</template>

<script setup lang="ts">
// /privacy: the privacy notice, a draft: what NovaWorld keeps and who handles
// it, in the sections the operator must write.
import LegalLayout from './LegalLayout.vue';
import LegalSection from './LegalSection.vue';
</script>
