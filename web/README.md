# Dual-Pool Proxy dashboard (by SerpentX)

`index.html` is the single-file dashboard the `splitter` serves on `:8080`. It is
self-contained (no build step, no external requests) and talks to the proxy via
`/api/status` (GET) and `/api/config` (POST). When those endpoints are
unreachable it renders built-in demo data so it can be previewed standalone.

## Branding assets (optional drop-ins)

Brand assets (canonical names the page loads, auto-detected at runtime):

- `serpentx-emblem.png` — **the header logo** (the mecha crest, which already
  contains the SERPENTX word). Shown ~76px tall; hides the CSS wordmark when
  present. Currently: the "SerpentX Gundam Small Logo".
- `serpentx-favicon.png` — browser-tab / apple-touch icon. Currently: the
  "Chubi Gundam" mascot (reads well at small sizes).
- `serpentx-mech.png` — *optional* art shown as a faint (~10% opacity) watermark
  in the top-right of the header. Hidden if absent.

If no `serpentx-emblem.png` is present the header falls back to a two-tone CSS
wordmark (amber-top / brown-bottom, black outline). The original source art
("SerpentX Gundam Small Logo.png", "SerpentX Word only.png", "Chubi Gundam.png")
is kept in this folder as the master copies.

Palette used throughout (from the SerpentX logo): amber `#FDB515` / `#F5A623`,
deep amber `#C8821A`, brown `#7A4A16` / `#8B5A1F`, black outline `#141414`, on a
dark mecha-grey base `#1b1d21`.
