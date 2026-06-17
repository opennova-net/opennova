<template>
  <main class="space-y-24 bg-surface">
    <section class="px-6 pt-24 pb-24">
      <div class="mx-auto max-w-4xl text-center">
        <p class="text-sm uppercase tracking-[0.3em] text-accent">Community tools</p>
        <h1 class="mt-4 text-4xl font-bold text-ink sm:text-5xl">Tools</h1>
        <p class="mt-6 max-w-3xl mx-auto text-lg text-ink-muted">
          OpenNova is built around a portable C++ core that reads and writes the original
          NovaLogic formats directly: terrain, 3DI models, missions, menus, audio, and the
          PFF archives that hold them. The OpenNova Editor (ONED) puts that core to work
          with workspaces for terrain painting, mission authoring, environment setup, and
          asset browsing, so you can open a retail install of Joint Operations or a newer
          title and start editing what shipped.
        </p>
        <p class="mt-4 max-w-3xl mx-auto text-lg text-ink-muted">
          Everything is pre-1.0, open source, and under active development. Editor and
          runtime builds for Windows and macOS are published from CI on every release.
          The same repository carries the Blender and 3ds Max exchange tools and the
          Python utilities used to take assets apart and put them back together. Start
          with the README and the docs folder for current workflows and known limits.
        </p>
        <div class="mt-8 flex flex-wrap items-center justify-center gap-4">
          <a
            href="https://github.com/opennova-net/opennova/releases"
            target="_blank"
            rel="noopener"
            class="inline-flex items-center gap-2 rounded-control bg-accent px-6 py-3 font-semibold text-on-accent transition hover:bg-accent/90"
          >
            Get the Editor
          </a>
          <a
            href="https://github.com/opennova-net/opennova"
            target="_blank"
            rel="noopener"
            class="inline-flex items-center gap-2 rounded-control border border-border-strong px-6 py-3 font-semibold text-ink transition hover:border-accent hover:text-ink-bright"
          >
            View on GitHub
          </a>
        </div>
      </div>
    </section>
  </main>
</template>
