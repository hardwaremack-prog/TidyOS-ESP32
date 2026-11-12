// TidyOS spell check: ~78,000-word English dictionary kept in flash, plus your own words
#pragma once
#include <string>
#include <vector>

void spellBegin();                                   // loads "my words" from storage
bool spellIsWord(const std::string & word);          // true = spelled right (or not a word to check)
void spellSuggest(const std::string & word, std::vector<std::string> & out, int maxCount = 6);
void spellAddWord(const std::string & word);         // add to "my words" (saved)
void spellIgnore(const std::string & word);          // ignore until restart

// helper: find word boundaries.  isWordChar() for letters and apostrophes inside words
inline bool spellWordChar(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '\''; }
