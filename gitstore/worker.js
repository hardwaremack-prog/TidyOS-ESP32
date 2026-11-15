// TidyOS Git-Store relay - a free Cloudflare Worker.
// It keeps the GitHub token (boards never see it), gives each board its own folder
// (boards/TidyOS-XXXXXX in your repo) and enforces the size limits.
//
// Worker settings (Settings > Variables and Secrets):
//   GITHUB_TOKEN  (Secret)  a fine-grained token with "Contents: Read and write" on ONE repo
//   REPO          (Text)    "owner/name", e.g. "hardwaremack-prog/tidyos-store"
//   MAX_BOARD_MB  (Text, optional)  per-board limit, default 5
//   MAX_FILE_MB   (Text, optional)  per-file limit, default 2
//   MAX_REPO_MB   (Text, optional)  stop all uploads once the repo is this big, default 900

const ID = /^TidyOS-[0-9A-F]{6}$/;
const NAME = /^[A-Za-z0-9 ._()-]{1,80}$/;
const MB = 1048576;

function cors(r) {
  const h = new Headers(r.headers);
  h.set('Access-Control-Allow-Origin', '*');
  h.set('Access-Control-Allow-Methods', 'GET, PUT, DELETE, OPTIONS');
  h.set('Access-Control-Allow-Headers', 'Content-Type');
  return new Response(r.body, { status: r.status, headers: h });
}
function json(o, status = 200) {
  return cors(new Response(JSON.stringify(o), { status, headers: { 'Content-Type': 'application/json' } }));
}
function gh(env, path, init = {}) {
  init.headers = Object.assign({
    'Authorization': 'Bearer ' + env.GITHUB_TOKEN,
    'Accept': 'application/vnd.github+json',
    'User-Agent': 'tidyos-store',
    'X-GitHub-Api-Version': '2022-11-28',
  }, init.headers || {});
  return fetch('https://api.github.com/repos/' + env.REPO + path, init);
}
async function listFiles(env, board) {
  const r = await gh(env, '/contents/boards/' + board);
  if (r.status === 404) return [];
  if (!r.ok) throw new Error('GitHub said ' + r.status);
  const a = await r.json();
  return a.filter(x => x.type === 'file').map(x => ({ name: x.name, size: x.size, sha: x.sha }));
}
function base64(buf) {
  const u = new Uint8Array(buf);
  let s = '';
  for (let i = 0; i < u.length; i += 0x8000) s += String.fromCharCode.apply(null, u.subarray(i, i + 0x8000));
  return btoa(s);
}

export default {
  async fetch(req, env) {
    if (req.method === 'OPTIONS') return cors(new Response(null, { status: 204 }));
    const u = new URL(req.url);
    const board = u.searchParams.get('board') || '', name = u.searchParams.get('name') || '';
    const maxBoard = (+env.MAX_BOARD_MB || 5) * MB, maxFile = (+env.MAX_FILE_MB || 2) * MB, maxRepo = (+env.MAX_REPO_MB || 900) * MB;
    try {
      if (u.pathname === '/' || u.pathname === '/ping') return json({ ok: true, service: 'tidyos-store' });
      if (!env.GITHUB_TOKEN || !env.REPO) return json({ error: 'relay not set up (GITHUB_TOKEN / REPO)' }, 500);
      if (!ID.test(board)) return json({ error: 'bad board id' }, 400);
      if (u.pathname === '/list') {
        const f = await listFiles(env, board);
        return json({ files: f.map(x => ({ name: x.name, size: x.size })), used: f.reduce((a, x) => a + x.size, 0), max: maxBoard });
      }
      if (u.pathname !== '/file') return json({ error: 'not found' }, 404);
      if (!NAME.test(name) || name.startsWith('.')) return json({ error: 'bad file name' }, 400);
      const path = '/contents/boards/' + board + '/' + encodeURIComponent(name);
      if (req.method === 'GET') {
        const r = await gh(env, path, { headers: { 'Accept': 'application/vnd.github.raw' } });
        if (!r.ok) return json({ error: 'not found' }, 404);
        return cors(new Response(r.body, { headers: { 'Content-Type': 'application/octet-stream' } }));
      }
      if (req.method === 'DELETE') {
        const f = (await listFiles(env, board)).find(x => x.name === name);
        if (!f) return json({ error: 'not found' }, 404);
        const r = await gh(env, path, { method: 'DELETE', body: JSON.stringify({ message: board + ': delete ' + name, sha: f.sha }) });
        return r.ok ? json({ ok: true }) : json({ error: 'GitHub said ' + r.status }, 502);
      }
      if (req.method === 'PUT') {
        if (+(req.headers.get('Content-Length') || 0) > maxFile) return json({ error: 'file too big (limit ' + maxFile / MB + ' MB)' }, 413);
        const body = await req.arrayBuffer();
        if (body.byteLength > maxFile) return json({ error: 'file too big (limit ' + maxFile / MB + ' MB)' }, 413);
        const files = await listFiles(env, board), old = files.find(x => x.name === name);
        const used = files.reduce((a, x) => a + x.size, 0) - (old ? old.size : 0);
        if (used + body.byteLength > maxBoard) return json({ error: 'over the ' + maxBoard / MB + ' MB limit for this board' }, 413);
        const info = await gh(env, '');
        if (info.ok && (await info.json()).size * 1024 > maxRepo) return json({ error: 'the store is full' }, 507);
        const r = await gh(env, path, { method: 'PUT', body: JSON.stringify({ message: board + ': ' + name, content: base64(body), sha: old ? old.sha : undefined }) });
        if (!r.ok) return json({ error: 'GitHub said ' + r.status }, 502);
        return json({ ok: true, used: used + body.byteLength, max: maxBoard });
      }
      return json({ error: 'method not allowed' }, 405);
    } catch (e) {
      return json({ error: String((e && e.message) || e) }, 500);
    }
  },
};
