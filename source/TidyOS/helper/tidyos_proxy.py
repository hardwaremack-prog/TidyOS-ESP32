#!/usr/bin/env python3
"""
TidyOS browser helper
---------------------
Runs on a Raspberry Pi (or The Shelf, or any computer on your home network).
It also prints for TidyOS (POST /print) using CUPS, so any printer the Pi can
print to works.

The ESP32 asks it for a page; the helper downloads it (https, big pages,
redirects, gzip - all the heavy work), turns it into plain text with numbered
links, and sends back a small text reply the ESP32 can show.

    python3 tidyos_proxy.py              # listens on port 8088
    python3 tidyos_proxy.py --port 9000

Then in TidyOS: Settings -> Browser -> Helper address  ->  <pi-ip>:8088

Only the Python standard library is used - nothing to install.
"""
import argparse
import gzip
import html
import re
import os
import shutil
import socket
import subprocess
import tempfile
import unicodedata
import urllib.error
import urllib.parse
import urllib.request
import zlib
from html.parser import HTMLParser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

MAX_TEXT = 15000          # characters of page text sent to the ESP32
MAX_LINKS = 250
MAX_DOWNLOAD = 3_000_000  # bytes
UA = "Mozilla/5.0 (X11; Linux) TidyOS-helper/1.0 (text browser for ESP32)"

SKIP = {"script", "style", "noscript", "svg", "template", "select", "nav", "aside", "footer", "iframe", "canvas"}
BLOCK = {"p", "div", "ul", "ol", "tr", "table", "section", "article", "header", "blockquote", "form",
         "dl", "dt", "dd", "main", "figure", "figcaption", "details", "summary"}
VOID = {"br", "img", "hr", "input", "meta", "link", "area", "base", "col", "embed", "source", "track", "wbr"}

SMART = {"‘": "'", "’": "'", "“": '"', "”": '"', "–": "-", "—": "-",
         "…": "...", "•": "*", " ": " ", "©": "(c)", "®": "(R)", "™": "(TM)",
         "°": " deg", "€": "EUR", "£": "GBP"}


def to_ascii(s):
    out = []
    for ch in s:
        if ord(ch) < 128:
            out.append(ch)
        elif ch in SMART:
            out.append(SMART[ch])
        else:
            d = unicodedata.normalize("NFKD", ch).encode("ascii", "ignore").decode()
            out.append(d)
    return "".join(out)


def unwrap(url):
    """Unwrap DuckDuckGo / Google redirect links so the ESP32 gets the real address."""
    try:
        p = urllib.parse.urlparse(url)
        q = urllib.parse.parse_qs(p.query)
        if "duckduckgo.com" in p.netloc and p.path.startswith("/l/") and "uddg" in q:
            return q["uddg"][0]
        if "google." in p.netloc and p.path == "/url" and "q" in q:
            return q["q"][0]
    except Exception:
        pass
    return url


class TextMaker(HTMLParser):
    def __init__(self, base):
        super().__init__(convert_charrefs=True)
        self.base = base
        self.parts = []
        self.size = 0
        self.links = []
        self.title = ""
        self.in_title = False
        self.skip = 0
        self.skip_tag = []
        self.pre = 0
        self.cur_link = 0
        self.nl = 2
        self.space = True

    # ---- output helpers
    def out(self, s):
        if self.size >= MAX_TEXT:
            return
        for ch in s:
            if ch == "\n":
                if self.nl >= 2:
                    continue
                while self.parts and self.parts[-1] == " ":
                    self.parts.pop()
                self.parts.append("\n")
                self.nl += 1
                self.space = True
            elif ch in " \t\r" and not self.pre:
                if self.space:
                    continue
                self.parts.append(" ")
                self.space = True
            else:
                self.parts.append(ch)
                self.nl = 0
                self.space = False
            self.size += 1

    def newline(self, n=1):
        for _ in range(n):
            self.out("\n")

    # ---- parser callbacks
    def handle_starttag(self, tag, attrs):
        if self.skip:
            if tag == self.skip_tag[-1] and tag not in VOID:
                self.skip += 1
            return
        if tag in SKIP:
            self.skip = 1
            self.skip_tag.append(tag)
            return
        a = dict(attrs)
        if tag == "title":
            self.in_title = True
        elif tag == "a":
            href = a.get("href") or ""
            if href and not href.startswith(("#", "javascript:", "mailto:", "tel:")) and len(self.links) < MAX_LINKS:
                self.links.append(unwrap(urllib.parse.urljoin(self.base, href)))
                self.cur_link = len(self.links)
            else:
                self.cur_link = 0
        elif tag == "br":
            self.out("\n")
            self.nl = 0
        elif tag == "li":
            self.newline()
            self.out(" * ")
        elif tag == "hr":
            self.newline()
            self.out("-" * 40)
            self.newline()
        elif tag in ("td", "th"):
            self.out("  ")
        elif tag == "img":
            alt = (a.get("alt") or "").strip()
            if alt and len(alt) < 60:
                self.out("[img: %s] " % to_ascii(alt))
        elif tag == "pre":
            self.pre += 1
            self.newline()
        elif re.fullmatch(r"h[1-6]", tag):
            self.newline(2)
            if tag in ("h1", "h2"):
                self.out("== ")
        elif tag in BLOCK:
            self.newline(2 if tag == "p" else 1)

    def handle_endtag(self, tag):
        if self.skip:
            if tag == self.skip_tag[-1]:
                self.skip -= 1
                if self.skip == 0:
                    self.skip_tag.pop()
            return
        if tag == "title":
            self.in_title = False
        elif tag == "a" and self.cur_link:
            self.out("[%d] " % self.cur_link)
            self.cur_link = 0
        elif tag == "pre":
            self.pre = max(0, self.pre - 1)
            self.newline()
        elif re.fullmatch(r"h[1-6]", tag):
            self.newline()
        elif tag in BLOCK:
            self.newline(2 if tag == "p" else 1)

    def handle_data(self, data):
        if self.skip:
            return
        if self.in_title:
            self.title += data
            return
        self.out(to_ascii(data))

    def result(self, final_url):
        text = "".join(self.parts).strip()
        title = to_ascii(" ".join(self.title.split()))[:120]
        lines = ["TITLE: " + title, "URL: " + final_url, "---", text, "#LINKS#"]
        lines += ["%d %s" % (i + 1, u) for i, u in enumerate(self.links)]
        return "\n".join(lines) + "\n"


def fetch(url):
    req = urllib.request.Request(url, headers={
        "User-Agent": UA,
        "Accept": "text/html,text/plain;q=0.9,*/*;q=0.5",
        "Accept-Encoding": "gzip, deflate",
        "Accept-Language": "en-US,en;q=0.8",
    })
    with urllib.request.urlopen(req, timeout=20) as r:
        raw = r.read(MAX_DOWNLOAD)
        enc = (r.headers.get("Content-Encoding") or "").lower()
        if enc == "gzip":
            raw = gzip.decompress(raw)
        elif enc == "deflate":
            try:
                raw = zlib.decompress(raw)
            except zlib.error:
                raw = zlib.decompress(raw, -zlib.MAX_WBITS)
        ctype = r.headers.get("Content-Type") or "text/html"
        charset = r.headers.get_content_charset()
        final = r.geturl()
    if not charset:
        m = re.search(rb'<meta[^>]+charset=["\']?([A-Za-z0-9_\-]+)', raw[:4000], re.I)
        charset = m.group(1).decode() if m else "utf-8"
    try:
        body = raw.decode(charset, "replace")
    except LookupError:
        body = raw.decode("utf-8", "replace")
    return final, ctype, body


def page_for(url):
    if not re.match(r"^https?://", url, re.I):
        url = "http://" + url
    try:
        final, ctype, body = fetch(url)
    except urllib.error.HTTPError as e:
        return "TITLE: Error\nURL: %s\n---\nThe site answered with error %d (%s).\n#LINKS#\n" % (url, e.code, e.reason)
    except (urllib.error.URLError, socket.timeout, ConnectionError) as e:
        return "TITLE: Error\nURL: %s\n---\nCould not load the page:\n%s\n#LINKS#\n" % (url, getattr(e, "reason", e))
    if "html" not in ctype.lower() and "xml" not in ctype.lower():
        if ctype.lower().startswith("text/"):
            return "TITLE: \nURL: %s\n---\n%s\n#LINKS#\n" % (final, to_ascii(body[:MAX_TEXT]))
        return "TITLE: File\nURL: %s\n---\nThis address is a file (%s), not a web page.\n#LINKS#\n" % (final, ctype)
    tm = TextMaker(final)
    try:
        tm.feed(body)
        tm.close()
    except Exception as e:  # broken HTML: keep what we have
        print("parse problem:", e)
    return tm.result(final)


INDEX = """<html><body style="font-family:sans-serif">
<h2>TidyOS browser helper is running</h2>
<p>In TidyOS open <b>Settings &rarr; Browser</b> and set the helper address to
<code>%s</code>.</p>
<form action="/fetch"><input name="url" size="50" value="https://example.com">
<button>Test</button></form></body></html>"""


def print_text(title, text, printer=None):
    """Print plain text with CUPS (the Pi's normal printing system)."""
    lp = shutil.which("lp")
    if not lp:
        return False, "CUPS isn't installed on this computer.  Run:  sudo apt install cups  and add your printer."
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
        f.write(text)
        name = f.name
    cmd = [lp, "-t", title[:60] or "TidyOS", "-o", "cpi=12", "-o", "lpi=7", "-o", "page-top=36", "-o", "page-left=36"]
    if printer:
        cmd += ["-d", printer]
    try:
        r = subprocess.run(cmd + [name], capture_output=True, text=True, timeout=30)
    finally:
        os.unlink(name)
    if r.returncode != 0:
        return False, (r.stderr or r.stdout or "lp failed").strip()[:300]
    return True, ("Sent to the printer. " + r.stdout.strip())[:300]


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"
    printer = None

    def do_POST(self):
        p = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(p.query)
        if p.path != "/print":
            self.send_error(404)
            return
        n = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(min(n, 2_000_000)).decode("utf-8", "replace")
        ok, msg = print_text(q.get("title", ["TidyOS"])[0], body, Handler.printer)
        data = msg.encode()
        self.send_response(200 if ok else 500)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        p = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(p.query)
        if p.path == "/fetch" and "url" in q:
            text = page_for(q["url"][0]).encode("ascii", "replace")
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=us-ascii")
            self.send_header("Content-Length", str(len(text)))
            self.end_headers()
            self.wfile.write(text)
            return
        host = self.headers.get("Host") or "this-computer:8088"
        body = (INDEX % html.escape(host)).encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        print("%s  %s" % (self.address_string(), fmt % args))


def main():
    ap = argparse.ArgumentParser(description="TidyOS browser helper")
    ap.add_argument("--port", type=int, default=8088)
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--printer", default=None, help="CUPS printer name for TidyOS printouts (default printer if left out)")
    a = ap.parse_args()
    Handler.printer = a.printer
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("10.255.255.255", 1))
        ip = s.getsockname()[0]
        s.close()
    except OSError:
        ip = "this-computer"
    print("TidyOS browser helper on port %d" % a.port)
    print("In TidyOS Settings use helper address:  %s:%d" % (ip, a.port))
    ThreadingHTTPServer((a.host, a.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
