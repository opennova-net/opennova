<template>
  <AuthCard id="forgot" title="Forgot your password?">
    <template v-if="sent">
      <Notice tone="ok" role="status">If that address has an account, we sent it a link to choose a new password.</Notice>
      <p class="text-sm text-ink-muted">
        Nothing arrived? <button type="button" class="link" @click="sent = false">Send it again</button>
      </p>
    </template>

    <form v-else class="grid gap-5" novalidate @submit.prevent="handleSubmit">
      <div>
        <label for="forgot-email" class="field-label">Your account's email</label>
        <input
          id="forgot-email"
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
          class="field"
        />
      </div>
      <button type="submit" :disabled="busy" class="btn-primary">{{ busy ? 'Sending…' : 'Send the link' }}</button>
      <Notice v-if="error" id="forgot-error" tone="bad" role="alert">{{ error }}</Notice>
    </form>

    <template #footer>
      <p><RouterLink to="/login" class="link">Back to log in</RouterLink></p>
    </template>
  </AuthCard>
</template>

<script setup lang="ts">
// /forgot: asks POST /api/auth/forgot to mail the account's reset link. The
// answer is the same for every address (the server tells nobody whether an
// account has it), so the page says so in those words.
import { onMounted, ref } from 'vue';
import { RouterLink } from 'vue-router';
import AuthCard from '../components/AuthCard.vue';
import Notice from '../components/ui/Notice.vue';
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
