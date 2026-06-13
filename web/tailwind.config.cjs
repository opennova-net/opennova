module.exports = {
  content: ['./index.html', './src/**/*.{vue,js,ts,jsx,tsx}'],
  theme: {
    extend: {
      colors: {
        // ONED "tactical tool" palette. Friendly names map to CSS variables in
        // src/assets/tailwind.css (the single place to re-skin the whole site).
        surface: 'rgb(var(--surface) / <alpha-value>)',
        panel: 'rgb(var(--panel) / <alpha-value>)',
        raised: 'rgb(var(--raised) / <alpha-value>)',
        hover: 'rgb(var(--hover) / <alpha-value>)',
        border: 'rgb(var(--border) / <alpha-value>)',
        'border-strong': 'rgb(var(--border-strong) / <alpha-value>)',
        ink: {
          DEFAULT: 'rgb(var(--ink) / <alpha-value>)',
          muted: 'rgb(var(--ink-muted) / <alpha-value>)',
          bright: 'rgb(var(--ink-bright) / <alpha-value>)'
        },
        'on-accent': 'rgb(var(--on-accent) / <alpha-value>)',
        accent: 'rgb(var(--accent) / <alpha-value>)',
        danger: 'rgb(var(--danger) / <alpha-value>)',
        warn: 'rgb(var(--warn) / <alpha-value>)',
        online: 'rgb(var(--online) / <alpha-value>)',
        // Back-compat alias: any un-swept brand-* renders as accent, never blue.
        brand: {
          50: 'rgb(var(--accent) / <alpha-value>)',
          100: 'rgb(var(--accent) / <alpha-value>)',
          200: 'rgb(var(--accent) / <alpha-value>)',
          500: 'rgb(var(--accent) / <alpha-value>)',
          700: 'rgb(var(--accent) / <alpha-value>)'
        }
      },
      borderColor: {
        // Bare `border` (no explicit color) uses the ONED 1px border color.
        DEFAULT: 'rgb(var(--border) / <alpha-value>)'
      },
      borderRadius: {
        // Semantic radius scale matching the editor StyleBoxes.
        control: '2px', // buttons, inputs, chips, list items
        panel: '4px', // cards, panels, tables, popups
        window: '6px' // modal / dialog shells
      }
    }
  },
  plugins: []
};
