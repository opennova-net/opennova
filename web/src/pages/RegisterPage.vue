<template>
  <AuthCard id="register" title="Register">
    <template #subtitle>One account for this site and your games.</template>
    <form class="grid gap-5" novalidate @submit.prevent="handleSubmit">
      <div>
        <label for="register-username" class="field-label">Username</label>
        <input
          id="register-username"
          v-model="form.username"
          type="text"
          required
          minlength="2"
          maxlength="30"
          autocomplete="username"
          autocapitalize="none"
          spellcheck="false"
          class="field"
        />
      </div>
      <div>
        <label for="register-email" class="field-label">Email</label>
        <input
          id="register-email"
          ref="emailInput"
          v-model="form.email"
          name="email"
          type="email"
          required
          maxlength="254"
          autocomplete="email"
          autocapitalize="none"
          spellcheck="false"
          :aria-invalid="emailProblem ? 'true' : undefined"
          :aria-describedby="emailProblem ? 'register-email-hint' : undefined"
          class="field"
        />
        <p v-if="emailProblem" id="register-email-hint" class="mt-1 text-xs text-danger">{{ emailProblem }}</p>
      </div>
      <div>
        <label for="register-password" class="field-label">Password</label>
        <input
          id="register-password"
          v-model="form.password"
          type="password"
          required
          minlength="3"
          maxlength="72"
          autocomplete="new-password"
          class="field"
        />
      </div>
      <button type="submit" :disabled="busy" class="btn-primary">{{ busy ? 'Registering…' : 'Register' }}</button>
      <Notice v-if="success" tone="ok" role="status" class="flex flex-wrap items-center justify-between gap-3">
        <p>Account created for <strong>{{ success.username }}</strong>.</p>
        <RouterLink ref="loginOffer" :to="loginLink(success.username)" class="btn-primary btn-sm">Log in</RouterLink>
      </Notice>
      <Notice v-if="error" tone="bad" role="alert">{{ error }}</Notice>
    </form>
    <template #footer>
      <p>Have an account? <RouterLink :to="loginLink()" class="link">Log in</RouterLink></p>
    </template>
  </AuthCard>
</template>

<script setup lang="ts">
// /register: a new NovaWorld account, then the way to log in with it.
import { nextTick, reactive, ref } from 'vue';
import { RouterLink, useRoute, type RouteLocationRaw } from 'vue-router';
import AuthCard from '../components/AuthCard.vue';
import Notice from '../components/ui/Notice.vue';
import { registerUser } from '../api/users';
import { safeRedirect } from '../router/guards';
import type { User } from '../types/users';
import { EMAIL_HINT, isEmailAddress } from '../utils/email';

const route = useRoute();

// The login page, carrying on the ?redirect= this page was opened with and,
// after a registration, the new username.
function loginLink(username?: string): RouteLocationRaw {
  const query: Record<string, string> = {};
  const redirect = safeRedirect(route.query.redirect);
  if (redirect !== '/') query.redirect = redirect;
  if (username) query.username = username;
  return { path: '/login', query };
}

const loginOffer = ref<{ $el: HTMLElement } | null>(null);

const form = reactive({ username: '', email: '', password: '' });
const busy = ref(false);
const success = ref<User | null>(null);
const error = ref<string | null>(null);
// What is wrong with the email field: the address rule, checked before the
// request, or the server's refusal of it. Shown under the field.
const emailProblem = ref<string | null>(null);
const emailInput = ref<HTMLInputElement | null>(null);

// The server's refusals in words; any other answer shows its own message.
function refusalText(code: string | undefined, message: string | undefined): string | null {
  switch (code) {
    case 'username_exists':
      return 'Another account already has that username.';
    case 'email_exists':
      return 'Another account already uses that email address.';
    case 'missing_field':
      return 'Fill in the username, the email and the password.';
    case 'rate_limited':
      return 'Too many registrations from your address. Try again in a few minutes.';
    default:
      return message || code || null;
  }
}

async function handleSubmit() {
  error.value = null;
  emailProblem.value = null;
  success.value = null;
  const email = form.email.trim();
  if (!isEmailAddress(email)) {
    emailProblem.value = EMAIL_HINT;
    emailInput.value?.focus();
    return;
  }
  busy.value = true;
  try {
    success.value = await registerUser({
      username: form.username.trim(),
      password: form.password,
      email,
    });
    form.username = '';
    form.email = '';
    form.password = '';
    // The next step is the login: put the keyboard on it.
    await nextTick();
    loginOffer.value?.$el.focus();
  } catch (e) {
    const detail = (e as { response?: { data?: { error?: string; message?: string } } })
      ?.response?.data;
    const text = refusalText(detail?.error, detail?.message);
    const aboutEmail =
      detail?.error === 'email_exists' || (detail?.error === 'invalid_field' && /email/i.test(detail?.message ?? ''));
    if (aboutEmail) {
      emailProblem.value = text;
      emailInput.value?.focus();
    } else {
      error.value = text || (e as Error).message || 'Registration failed.';
    }
  } finally {
    busy.value = false;
  }
}
</script>
