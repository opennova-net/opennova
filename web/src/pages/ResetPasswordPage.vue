<template>
  <AuthCard id="reset" title="Choose a new password">
    <template v-if="done">
      <Notice tone="ok" role="status">Your new password is set. Log in again with it, in the game too.</Notice>
      <div><RouterLink ref="loginOffer" to="/login" class="btn-primary">Log in</RouterLink></div>
    </template>

    <template v-else-if="linkDead">
      <Notice tone="bad" role="alert">This link has expired or was used already.</Notice>
      <p class="text-sm"><RouterLink to="/forgot" class="link">Ask for a new link</RouterLink></p>
    </template>

    <form v-else class="grid gap-5" novalidate @submit.prevent="handleSubmit">
      <div>
        <label for="reset-password" class="field-label">New password</label>
        <input
          id="reset-password"
          ref="passwordInput"
          v-model="password"
          name="new-password"
          type="password"
          autocomplete="new-password"
          required
          minlength="3"
          maxlength="72"
          class="field"
        />
      </div>
      <div>
        <label for="reset-repeat" class="field-label">New password, again</label>
        <input
          id="reset-repeat"
          ref="repeatInput"
          v-model="repeat"
          name="repeat-password"
          type="password"
          autocomplete="new-password"
          required
          maxlength="72"
          :aria-invalid="mismatch ? 'true' : undefined"
          :aria-describedby="error ? 'reset-error' : undefined"
          class="field"
        />
      </div>
      <button type="submit" :disabled="busy" class="btn-primary">{{ busy ? 'Setting it…' : 'Set the password' }}</button>
      <Notice v-if="error" id="reset-error" tone="bad" role="alert">{{ error }}</Notice>
    </form>
  </AuthCard>
</template>

<script setup lang="ts">
// /reset?token=<64 hex digits>: the link a password reset mail carries. The
// new password, typed twice, goes to POST /api/auth/reset with the token; a
// link that is malformed, spent or past its hour says so and offers a new one.
import { nextTick, onMounted, ref } from 'vue';
import { RouterLink, useRoute } from 'vue-router';
import AuthCard from '../components/AuthCard.vue';
import Notice from '../components/ui/Notice.vue';
import { isResetToken, resetPassword } from '../api/passwordReset';
import { useSession } from '../composables/useSession';

const route = useRoute();
const { refresh } = useSession();
const token = route.query.token;

const password = ref('');
const repeat = ref('');
const busy = ref(false);
const done = ref(false);
const linkDead = ref(!isResetToken(token));
const mismatch = ref(false);
const error = ref<string | null>(null);
const passwordInput = ref<HTMLInputElement | null>(null);
const repeatInput = ref<HTMLInputElement | null>(null);
const loginOffer = ref<{ $el: HTMLElement } | null>(null);

onMounted(() => passwordInput.value?.focus());

async function handleSubmit() {
  if (busy.value || !isResetToken(token)) return;
  error.value = null;
  mismatch.value = false;
  if (password.value === '') {
    error.value = 'Enter the new password.';
    passwordInput.value?.focus();
    return;
  }
  if (password.value !== repeat.value) {
    mismatch.value = true;
    error.value = 'The two passwords do not match.';
    repeatInput.value?.focus();
    return;
  }
  busy.value = true;
  const result = await resetPassword(token, password.value);
  busy.value = false;
  if (result.ok) {
    done.value = true;
    password.value = '';
    repeat.value = '';
    // The reset ended this browser's session too, if it had one.
    void refresh();
    await nextTick();
    loginOffer.value?.$el.focus();
    return;
  }
  if (result.error.code === 'invalid_token') {
    linkDead.value = true;
    return;
  }
  error.value = result.error.message;
}
</script>
