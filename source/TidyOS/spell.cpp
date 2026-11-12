// TidyOS spell check
#include <algorithm>
#include <set>
#include <string.h>
#include "spell.h"
#include "core.h"

extern const char   TIDY_DICT[];
extern const size_t TIDY_DICT_LEN;

static std::set<std::string> s_mine;     // "Add to dictionary"
static std::set<std::string> s_ignored;  // "Ignore" (this session)

// compare the dictionary word starting at p (ends at ' ') with w
static int cmpAt(const char * p, const char * w)
{
  while (*p != ' ' && *w) {
    if ((unsigned char)*p != (unsigned char)*w) return (unsigned char)*p < (unsigned char)*w ? -1 : 1;
    ++p; ++w;
  }
  if (*p == ' ' && !*w) return 0;
  return (*p == ' ') ? -1 : 1;
}

static bool inDict(const char * w)
{
  size_t lo = 0, hi = TIDY_DICT_LEN;
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    size_t s = mid;
    while (s > lo && TIDY_DICT[s - 1] != '\n') --s;          // start of that line
    int c = cmpAt(TIDY_DICT + s, w);
    if (c == 0) return true;
    if (c < 0) {
      const char * nl = (const char *)memchr(TIDY_DICT + s, '\n', TIDY_DICT_LEN - s);
      lo = nl ? (nl - TIDY_DICT) + 1 : TIDY_DICT_LEN;
    } else {
      hi = s;
    }
  }
  return false;
}

static std::string lower(const std::string & w)
{
  std::string l = w;
  for (auto & c : l) c = tolower((unsigned char)c);
  return l;
}

bool spellIsWord(const std::string & word)
{
  std::string w = word;
  while (!w.empty() && w[0] == '\'') w.erase(0, 1);
  while (!w.empty() && w.back() == '\'') w.pop_back();
  if (w.size() <= 1) return true;
  bool allCaps = true;
  for (char c : w) if (c >= 'a' && c <= 'z') allCaps = false;
  if (allCaps && w.size() <= 6) return true;                  // USB, NASA, OK...
  std::string l = lower(w);
  if (inDict(l.c_str())) return true;
  if (l.size() > 2 && l.compare(l.size() - 2, 2, "'s") == 0 && inDict(l.substr(0, l.size() - 2).c_str())) return true;   // possessive
  if (s_mine.count(l) || s_ignored.count(l)) return true;
  return false;
}

// restricted Damerau-Levenshtein distance, gives up above `limit`
static int distance(const char * a, int la, const char * b, int lb, int limit)
{
  if (abs(la - lb) > limit) return limit + 1;
  int d[3][34];
  if (la > 32 || lb > 32) return limit + 1;
  int * prev2 = d[0], * prev = d[1], * cur = d[2];
  for (int j = 0; j <= lb; ++j) prev[j] = j;
  for (int i = 1; i <= la; ++i) {
    cur[0] = i;
    int rowMin = cur[0];
    for (int j = 1; j <= lb; ++j) {
      int cost = a[i - 1] == b[j - 1] ? 0 : 1;
      int v = std::min(std::min(prev[j] + 1, cur[j - 1] + 1), prev[j - 1] + cost);
      if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1]) v = std::min(v, prev2[j - 2] + 1);
      cur[j] = v;
      rowMin = std::min(rowMin, v);
    }
    if (rowMin > limit) return limit + 1;
    int * t = prev2; prev2 = prev; prev = cur; cur = t;
  }
  return prev[lb];
}

void spellSuggest(const std::string & word, std::vector<std::string> & out, int maxCount)
{
  out.clear();
  std::string w = lower(word);
  int lw = w.size();
  if (lw < 2 || lw > 30) return;
  struct Cand { int score; const char * p; int len; };
  std::vector<Cand> best;
  const char * p = TIDY_DICT, * end = TIDY_DICT + TIDY_DICT_LEN;
  while (p < end) {
    const char * sp = (const char *)memchr(p, ' ', end - p);
    if (!sp) break;
    int len = sp - p;
    char band = sp[1];
    if (abs(len - lw) <= 2) {
      int dist = distance(w.c_str(), lw, p, len, 2);
      if (dist <= 2) {
        // closer first, then more common, then same first letter
        int score = dist * 100 + (band - 'a') * 3 + (p[0] == w[0] ? 0 : 20);
        if ((int)best.size() < maxCount * 3) best.push_back({ score, p, len });
        else {
          auto worst = std::max_element(best.begin(), best.end(), [](const Cand & a, const Cand & b) { return a.score < b.score; });
          if (score < worst->score) *worst = { score, p, len };
        }
      }
    }
    p = sp + 3;   // skip " f\n"
  }
  std::sort(best.begin(), best.end(), [](const Cand & a, const Cand & b) { return a.score < b.score; });
  bool cap = !word.empty() && isupper((unsigned char)word[0]);
  for (auto & c : best) {
    if ((int)out.size() >= maxCount) break;
    std::string s(c.p, c.len);
    if (cap) s[0] = toupper((unsigned char)s[0]);
    out.push_back(s);
  }
}

void spellBegin()
{
  std::string s;
  if (!readFileStr(dataPath("mywords.txt").c_str(), s)) return;
  size_t st = 0;
  for (size_t i = 0; i <= s.size(); ++i)
    if (i == s.size() || s[i] == '\n') {
      std::string w = s.substr(st, i - st);
      st = i + 1;
      if (!w.empty() && w.back() == '\r') w.pop_back();
      if (!w.empty()) s_mine.insert(lower(w));
    }
}

void spellAddWord(const std::string & word)
{
  std::string l = lower(word);
  if (l.empty() || s_mine.count(l)) return;
  s_mine.insert(l);
  std::string all;
  for (auto & w : s_mine) all += w + "\n";
  writeFileStr(dataPath("mywords.txt").c_str(), all);
}

void spellIgnore(const std::string & word) { s_ignored.insert(lower(word)); }
