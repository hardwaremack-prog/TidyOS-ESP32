#pragma once
// Small streaming helpers so documents are read from and written to files in 256-byte pieces
// instead of holding a second full copy of the file in RAM.
#include <stdio.h>
#include <string.h>
#include <string>

struct RSource {
  virtual ~RSource() {}
  virtual int  peek(int k = 0) = 0;     // byte k ahead of the read position, -1 at the end
  virtual void skip(int n = 1) = 0;
};

struct StrSource : RSource {
  const std::string & s;
  size_t i = 0;
  explicit StrSource(const std::string & str) : s(str) {}
  int  peek(int k = 0) override { return i + k < s.size() ? (unsigned char)s[i + k] : -1; }
  void skip(int n = 1) override { i = i + n > s.size() ? s.size() : i + n; }
};

struct FileSource : RSource {
  FILE * f;
  char   b[256];
  int    n = 0, pos = 0;
  bool   eof = false;
  explicit FileSource(FILE * file) : f(file) {}
  void fill() {
    if (eof) return;
    if (pos) { memmove(b, b + pos, n - pos); n -= pos; pos = 0; }
    size_t r = fread(b + n, 1, sizeof(b) - n, f);
    if (r == 0) eof = true;
    n += r;
  }
  int peek(int k = 0) override {
    while (pos + k >= n && !eof) fill();
    return pos + k < n ? (unsigned char)b[pos + k] : -1;
  }
  void skip(int m = 1) override { while (m-- > 0 && peek(0) >= 0) ++pos; }
};

struct RSink {
  virtual ~RSink() {}
  virtual void write(const char * p, size_t n) = 0;
  void put(char c) { write(&c, 1); }
  void put(const char * s) { write(s, strlen(s)); }
  void put(const std::string & s) { write(s.data(), s.size()); }
};

struct StrSink : RSink {
  std::string & o;
  explicit StrSink(std::string & out) : o(out) {}
  void write(const char * p, size_t n) override { o.append(p, n); }
};

struct FileSink : RSink {
  FILE * f;
  char   b[256];
  size_t n = 0;
  bool   ok = true;
  explicit FileSink(FILE * file) : f(file) {}
  void write(const char * p, size_t len) override {
    while (len) {
      size_t c = sizeof(b) - n < len ? sizeof(b) - n : len;
      memcpy(b + n, p, c); n += c; p += c; len -= c;
      if (n == sizeof(b)) flush();
    }
  }
  bool flush() {
    if (n && fwrite(b, 1, n, f) != n) ok = false;
    n = 0;
    return ok;
  }
};
