// The address rule the server applies (apps/novaworld_server/auth.h,
// normalize_email): at most 254 bytes, exactly one '@' with something before
// it, and a domain of dot-separated labels (at least two); no space or control
// character anywhere. The pages check it before they send, so a typo gets its
// message at once; the server's answer stays the word.

export const EMAIL_MAX_BYTES = 254;

export function isEmailAddress(text: string): boolean {
  if (text === '' || new TextEncoder().encode(text).length > EMAIL_MAX_BYTES) return false;
  if (/[\u0000- \u007f]/.test(text)) return false;
  const at = text.indexOf('@');
  if (at <= 0 || text.indexOf('@', at + 1) !== -1) return false;
  const labels = text.slice(at + 1).split('.');
  return labels.length >= 2 && labels.every((label) => label !== '');
}

export const EMAIL_HINT = 'Enter an email address such as name@example.com.';
