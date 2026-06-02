module.exports = {
  content: [
    './index.html',
    './src/**/*.{vue,js,ts,jsx,tsx}'
  ],
  theme: {
    extend: {
      colors: {
        brand: {
          50: '#f2f6ff',
          100: '#d9e4ff',
          200: '#adc2ff',
          500: '#356dff',
          700: '#1d3fbf'
        }
      }
    }
  },
  plugins: []
};
