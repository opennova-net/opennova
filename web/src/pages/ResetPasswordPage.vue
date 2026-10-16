<template>
  <section class="px-4 py-10 sm:px-6 sm:py-14">
    <div class="mx-auto max-w-md rounded-panel border border-border bg-panel p-8">
      <h1 class="text-3xl font-semibold text-ink">Choose a new password</h1>

      <template v-if="done">
        <p role="status" class="mt-6 rounded-panel border border-online/40 bg-online/10 p-3 text-sm text-online">
          Your new password is set. Every place the account was logged in is logged out; log in again with the
          new password, in the game too.
        </p>
        <div class="mt-6">
          <RouterLink
            ref="loginOffer"
            to="/login"
            class="rounded-control bg-accent px-4 py-2 font-semibold text-on-accent hover:bg-accent/90"
          >
            Log in
          </RouterLink>
        </div>
      </template>

      <template v-else-if="linkDead">
        <p role="alert" class="mt-6 rounded-panel border border-danger/40 bg-danger/10 p-3 text-sm text-danger">
          This link has expired or was used already. A link works for an hour, and once.
        </p>
        <p class="mt-6 text-sm text-ink-muted">
          <RouterLink to="/forgot" class="link">Ask for a new link</RouterLink>
        </p>
      </template>

      <template v-else>
        <p class="mt-2 text-sm text-ink-muted">
          It is the password you log in with, on this site and in the game.
        </p>
        <form class="mt-8 grid gap-6" novalidate @submit.prevent="handleSubmit">
          <label class="grid gap-2 text-sm text-ink">
            New password
            <input
              ref="passwordInput"
              v-model="password"
              name="new-password"
              type="password"
              autocomplete="new-password"
              required
              minlength="3"
              maxlength="72"
              class="rounded-control border border-ink-muted/60 bg-surface px-4 py-3 focus:border-accent focus:outline-none"
            />
          </label>
          <label class="grid gap-2 text-sm text-ink">
            New password, again
            <input
              ref="repeatInput"
              v-model="repeat"
              name="repeat-password"
              type="password"
              autocomplete="new-password"
              required
              maxlength="72"
              :aria-invalid="mismatch ? 'true' : undefined"
              :aria-describedby="error ? 'reset-error' : undefined"
              class="rounded-control border border-ink-muted/60 bg-surface px-4 py-3 focus:border-accent focus:outline-none"
            />
          </label>
          <button
            type="submit"
            :disabled="busy"
            class="rounded-control bg-accent px-6 py-3 font-semibold text-on-accent hover:bg-accent/90 disabled:opacity-50"
          >
            {{ busy ? 'Setting it…' : 'Set the password' }}
          </button>
          <p
            v-if="error"
            id="reset-error"
            role="alert"
            class="rounded-panel border border-danger/40 bg-danger/10 p-3 text-sm text-danger"
          >
            {{ error }}
          </p>
        </form>
      </template>
    </div>
  </section>
</template>

<script setup lang="ts">
// /reset?token=<64 hex digits>: the link a password reset mail carries. The
// new password, typed twice, goes to POST /api/auth/reset with the token; a
// link that is malformed, spent or past its hour says so and offers a new one.
import { nextTick, onMounted, ref } from 'vue';
import { RouterLink, useRoute } from 'vue-router';
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
