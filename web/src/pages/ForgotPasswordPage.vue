<template>
  <section class="px-4 py-10 sm:px-6 sm:py-14">
    <div class="mx-auto max-w-md rounded-panel border border-border bg-panel p-8">
      <h1 class="text-3xl font-semibold text-ink">Forgot your password?</h1>

      <template v-if="sent">
        <p role="status" class="mt-6 rounded-panel border border-online/40 bg-online/10 p-3 text-sm text-online">
          If that address has an account, we sent it a link to choose a new password. The link works for an
          hour, and once.
        </p>
        <p class="mt-4 text-sm text-ink-muted">
          Nothing in your inbox? Check the spam folder, or
          <button type="button" class="link" @click="sent = false">send it again</button>.
        </p>
      </template>

      <template v-else>
        <p class="mt-2 text-sm text-ink-muted">
          Enter your account's email address. We will mail it a link to choose a new password.
        </p>
        <form class="mt-8 grid gap-6" novalidate @submit.prevent="handleSubmit">
          <label class="grid gap-2 text-sm text-ink">
            Email
            <input
              ref="emailInput"
              v-model="email"
              name="email"
              type="email"
              autocomplete="email"
              autocapitalize="none"
              spellcheck="false"
              required
              maxlength="254"
              :aria-invalid="error ? 'true' : undefined"
              :aria-describedby="error ? 'forgot-error' : undefined"
              class="rounded-control border border-ink-muted/60 bg-surface px-4 py-3 focus:border-accent focus:outline-none"
            />
          </label>
          <button
            type="submit"
            :disabled="busy"
            class="rounded-control bg-accent px-6 py-3 font-semibold text-on-accent hover:bg-accent/90 disabled:opacity-50"
          >
            {{ busy ? 'Sending…' : 'Send the link' }}
          </button>
          <p
            v-if="error"
            id="forgot-error"
            role="alert"
            class="rounded-panel border border-danger/40 bg-danger/10 p-3 text-sm text-danger"
          >
            {{ error }}
          </p>
        </form>
      </template>

      <p class="mt-6 text-sm text-ink-muted">
        <RouterLink to="/login" class="link">Back to log in</RouterLink>
      </p>
    </div>
  </section>
</template>

<script setup lang="ts">
// /forgot: asks POST /api/auth/forgot to mail the account's reset link. The
// answer is the same for every address (the server tells nobody whether an
// account has it), so the page says so in those words.
import { onMounted, ref } from 'vue';
import { RouterLink } from 'vue-router';
import { requestPasswordReset } from '../api/passwordReset';
import { EMAIL_HINT, isEmailAddress } from '../utils/email';

const email = ref('');
const busy = ref(false);
const sent = ref(false);
const error = ref<string | null>(null);
const emailInput = ref<HTMLInputElement | null>(null);

onMounted(() => emailInput.value?.focus());

async function handleSubmit() {
  if (busy.value) return;
  error.value = null;
  const address = email.value.trim();
  if (!isEmailAddress(address)) {
    error.value = EMAIL_HINT;
    emailInput.value?.focus();
    return;
  }
  busy.value = true;
  const result = await requestPasswordReset(address);
  busy.value = false;
  if (result.ok) {
    sent.value = true;
    return;
  }
  error.value = result.error.message;
  emailInput.value?.focus();
}
</script>
