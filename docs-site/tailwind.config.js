import colors from 'tailwindcss/colors'

/** @type {import('tailwindcss').Config} */
export default {
  darkMode: 'class',
  content: [
    './index.html',
    './src/**/*.{js,jsx}',
    // rewind-ui components are styled against the `primary-*` palette
    './node_modules/rewind-ui/dist/*.js',
  ],
  theme: {
    extend: {
      colors: {
        // Untitled UI rich purple brand tokens
        primary: {
          50: '#f9f5ff',
          100: '#f4ebff',
          200: '#e9d7fe',
          300: '#d6bbfb',
          400: '#b692f6',
          500: '#9e77ed',
          600: '#7f56d9',
          700: '#6941c6',
          800: '#53389e',
          900: '#42307d',
          950: '#2c1c5f',
        },
        brand: {
          50: '#f9f5ff',
          100: '#f4ebff',
          200: '#e9d7fe',
          300: '#d6bbfb',
          400: '#b692f6',
          500: '#9e77ed',
          600: '#7f56d9',
          700: '#6941c6',
          800: '#53389e',
          900: '#42307d',
          950: '#2c1c5f',
        },
        dark: {
          bg: '#0a0714',
          card: '#120d24',
          surface: '#181230',
          border: '#2a1f4e',
          muted: '#8b85a3',
        },
      },
      fontFamily: {
        sans: ['Inter', 'system-ui', '-apple-system', 'BlinkMacSystemFont', '"Segoe UI"', 'Roboto', 'sans-serif'],
        mono: ['"JetBrains Mono"', 'ui-monospace', 'SFMono-Regular', 'Menlo', 'Consolas', 'monospace'],
      },
      boxShadow: {
        'glow-sm': '0 0 15px -3px rgba(158, 119, 237, 0.25)',
        'glow': '0 0 25px -5px rgba(158, 119, 237, 0.35)',
        'glow-lg': '0 0 40px -10px rgba(158, 119, 237, 0.45)',
      },
    },
  },
  plugins: [],
}
