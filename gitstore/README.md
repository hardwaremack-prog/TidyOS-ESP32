# Git-Store relay (Cloudflare Worker)

Git-Store gives every TidyOS board a small cloud folder, handy when a board has no SD card.
Files live in a GitHub repo you own, in `boards/TidyOS-XXXXXX` (the board's own number).
A free Cloudflare Worker sits in between: **it holds the GitHub token, so boards never see it**,
keeps each board in its own folder, and enforces the limits (2 MB a file, 5 MB a board, 900 MB for the whole repo).
The TidyOS web page does the transfers, so the ESP32 itself never needs an HTTPS connection.

## Set it up once (about 5 minutes)

1. **Make the store repo.** On GitHub: New repository, name it `tidyos-store`, choose **Private**, tick "Add a README", Create.
2. **Make a token that can only touch that repo.** GitHub > Settings > Developer settings > Personal access tokens > **Fine-grained tokens** > Generate new token.
   - Repository access: **Only select repositories** > `tidyos-store`
   - Permissions > Repository permissions > **Contents: Read and write** (leave everything else as is)
   - Generate, and copy the token (it starts with `github_pat_`).
3. **Make the worker.** Sign in at <https://dash.cloudflare.com> (free account) > Workers & Pages > Create > **Create Worker**. Name it `tidyos-store`, Deploy.
   Then **Edit code**, delete what's there, paste everything from [`worker.js`](worker.js), and **Deploy**.
4. **Give it the token.** In the worker: Settings > Variables and Secrets > Add:
   - `GITHUB_TOKEN`, type **Secret**, value: the token from step 2
   - `REPO`, type **Text**, value: `your-github-name/tidyos-store`
   Save and deploy.
5. **Optional, recommended: rate limit.** Your Cloudflare dashboard > Security > WAF > Rate limiting rules: e.g. 30 requests a minute per IP for the worker's address.
6. **Copy the worker's address** (like `https://tidyos-store.your-name.workers.dev`). Opening it in a browser should show `{"ok":true,"service":"tidyos-store"}`.

## On each board

Open **Git-Store** on the desktop, paste the worker address into **Relay**, press **Save**.
Upload sends a file from the board, Download puts one in Documents, Delete removes it.
The TidyOS page must be open in a browser (it carries the files).

## Limits

Change them with optional worker variables: `MAX_BOARD_MB` (default 5), `MAX_FILE_MB` (2), `MAX_REPO_MB` (900).
Board numbers can be made up by someone who knows the address, so keep the rate limit on;
each made-up number still only gets its own 5 MB, and nothing can touch any other folder or the rest of your GitHub.
