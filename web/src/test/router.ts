// A router for mounting a page in a test: every path renders nothing, so the
// page's RouterLinks resolve and a navigation shows in currentRoute. `patterns`
// name the routes whose params the page reads ('/admin/users/:id').

import { createMemoryHistory, createRouter, type Router } from 'vue-router';

export async function testRouter(path = '/', patterns: string[] = []): Promise<Router> {
  const blank = { render: () => null };
  const router = createRouter({
    history: createMemoryHistory(),
    routes: [...patterns.map((p) => ({ path: p, component: blank })), { path: '/:rest(.*)*', component: blank }],
  });
  await router.push(path);
  return router;
}
