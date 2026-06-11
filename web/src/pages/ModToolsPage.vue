<template>
  <main class="space-y-24 bg-slate-950">
    <section class="px-6 pt-24 pb-24">
      <div class="mx-auto max-w-4xl text-center">
        <p class="text-sm uppercase tracking-[0.3em] text-brand-200">Community tools</p>
        <h1 class="mt-4 text-4xl font-bold text-white sm:text-5xl">Tools</h1>
        <p class="mt-6 max-w-3xl mx-auto text-lg text-slate-300">
          OpenNova is built around a portable C++ core that reads and writes the original
          NovaLogic formats directly: terrain, 3DI models, missions, menus, audio, and the
          PFF archives that hold them. The OpenNova Editor (ONED) puts that core to work
          with workspaces for terrain painting, mission authoring, environment setup, and
          asset browsing, so you can open a retail install of Joint Operations or a newer
          title and start editing what shipped.
        </p>
        <p class="mt-4 max-w-3xl mx-auto text-lg text-slate-300">
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
            class="inline-flex items-center gap-2 rounded-full bg-brand-500 px-6 py-3 font-semibold text-white shadow-lg transition hover:bg-brand-400"
          >
            Get the Editor
          </a>
          <a
            href="https://github.com/opennova-net/opennova"
            target="_blank"
            rel="noopener"
            class="inline-flex items-center gap-2 rounded-full border border-slate-600 px-6 py-3 font-semibold text-slate-200 transition hover:border-brand-400 hover:text-white"
          >
            View on GitHub
          </a>
        </div>
      </div>
    </section>
  </main>
</template>
