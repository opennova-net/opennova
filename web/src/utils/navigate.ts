// Leaving the site for Stripe: a Checkout or billing portal URL the service
// answered (api/billing.ts) is followed only when it is one of Stripe's own
// pages, so a reply that was tampered with (or a bug) can never send a
// visitor to another site. The pages call navigateToStripe; tests mock this
// module to see where a page would have gone.

const STRIPE_HOSTS: ReadonlySet<string> = new Set(['checkout.stripe.com', 'billing.stripe.com']);

// Whether `url` is an https page on Stripe's Checkout or billing portal host:
// no other scheme, host, port or credentials.
export function isStripeUrl(url: string): boolean {
  let parsed: URL;
  try {
    parsed = new URL(url);
  } catch {
    return false;
  }
  return (
    parsed.protocol === 'https:' &&
    STRIPE_HOSTS.has(parsed.hostname) &&
    parsed.port === '' &&
    parsed.username === '' &&
    parsed.password === ''
  );
}

// Goes to `url` when it is a Stripe page (true); else stays (false), and the
// page says something went wrong.
export function navigateToStripe(url: string, assign: (url: string) => void = (u) => window.location.assign(u)): boolean {
  if (!isStripeUrl(url)) return false;
  assign(url);
  return true;
}
