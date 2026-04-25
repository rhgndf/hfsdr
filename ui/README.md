# Svelte + Tailwind

This project was scaffolded with `create-vite` using the Svelte template, then configured with Tailwind CSS v4 via `@tailwindcss/vite`.

## Run

```bash
npm install
npm run dev
```

## Build

```bash
npm run build
npm run preview
```

## Deploy to GitHub Pages

This repository includes a GitHub Actions workflow at
`.github/workflows/deploy-ui-gh-pages.yml` that builds `ui/` and publishes the
`ui/dist` output to GitHub Pages.

### Deploy flow

- Automatic deploy: push changes to `main` that touch files under `ui/`.
- Manual deploy: run **Deploy UI to GitHub Pages** from the Actions tab.

The Vite base path is already set to `/hfsdr/` in `ui/vite.config.js`.
