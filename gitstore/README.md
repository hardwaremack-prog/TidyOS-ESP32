# Git-Store relay (Cloudflare Worker)

Git-Store gives every TidyOS board a small cloud folder - handy when a board has no SD card.
A free Cloudflare Worker keeps the files, gives each board its own folder (named after the board's
number, like `TidyOS-A1B2C3`) and enforces the limits: 25 MB a file, 1 GB a board.
The TidyOS web page does the transfers, so the ESP32 itself never needs an HTTPS connection.

**Only one person sets this up.** Its address is built into TidyOS, so everyone else just opens Git-Store.

## Set it up (about 3 minutes, free, no card needed)

1. Sign in (or sign up) at <https://dash.cloudflare.com>.
2. **Storage & Databases > KV > Create** a namespace called `tidyos-store`.
3. **Workers & Pages > Create > Create Worker**, name it `tidyos-store`, **Deploy**.
4. **Edit code**: delete what's there, paste everything from [`worker.js`](worker.js), **Deploy**.
5. In the worker: **Settings > Bindings > Add > KV namespace**: Variable name `STORE`, namespace `tidyos-store`. Save (it redeploys).
6. Open the worker's address (like `https://tidyos-store.your-name.workers.dev`). You should see `{"ok":true,"service":"tidyos-store"}`.
7. Optional, recommended: **Security > WAF > Rate limiting rules**, e.g. 30 requests a minute per IP for that address.

Send the worker's address to whoever builds TidyOS (it goes in `config.h` as `TIDYOS_STORE_URL`),
or paste it into **Relay** in the Git-Store window and press **Save**.

Free Cloudflare limits: 1 GB stored and 1,000 saves a day in total - plenty for notes and documents.

## Limits

Optional worker variables (Settings > Variables): `MAX_BOARD_MB` (default 1024), `MAX_FILE_MB` (default 25).
Someone who knows the address could make up board numbers, so keep the rate limit on; each made-up number
can only fill its own folder, and nothing can touch any other board's folder.

## Keeping files on GitHub instead (optional)

Leave out the KV binding and add two worker variables instead: `GITHUB_TOKEN` (Secret: a fine-grained token with
**Contents: Read and write** on one private repo only) and `REPO` (Text: `owner/name`). Files then go to
`boards/TidyOS-XXXXXX` in that repo.
