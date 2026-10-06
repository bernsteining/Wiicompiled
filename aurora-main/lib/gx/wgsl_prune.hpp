#pragma once
// Browser build only: drops the functions a generated GX shader never calls.
//
// Every GX shader carries the same ~12 KB prelude of helpers (vertex fetch for every attribute
// format, TEV arithmetic), while its own code is ~4 KB. A browser parses and validates the whole
// text in createShaderModule on the page's only thread: measured in Chrome, 1993 modules of 16 KB
// median took 7.9 ms each and left so much garbage that the collector ran half of the startup
// prewarm; Safari then translates and compiles all of it. Unused functions change nothing in the
// compiled shader, so they are removed first.
//
// The scan relies on how shader.cpp writes WGSL: a top-level function starts with "fn " at the
// start of a line and its body is brace-balanced (comments are skipped). Entry points and all other
// top-level text (structs, bindings, constants) are roots; a function is kept when a kept item
// names it.

#ifdef AURORA_WEBGPU_BROWSER

#include <cctype>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace aurora::gx {

inline std::string prune_unused_wgsl_functions(std::string_view source) {
  struct Function {
    std::string_view name;
    size_t begin; // start of the line holding "fn"
    size_t end;   // one past the closing brace's line
    bool kept = false;
  };
  std::vector<Function> functions;
  std::unordered_map<std::string_view, size_t> byName;

  for (size_t line = 0; line < source.size();) {
    const size_t lineEnd = std::min(source.find('\n', line), source.size());
    if (source.substr(line, 3) == "fn ") {
      size_t nameEnd = line + 3;
      while (nameEnd < source.size() && (std::isalnum(static_cast<unsigned char>(source[nameEnd])) || source[nameEnd] == '_')) {
        ++nameEnd;
      }
      // Body: from the first '{' to its matching '}', skipping comments.
      size_t at = source.find('{', nameEnd);
      int depth = 0;
      for (; at < source.size(); ++at) {
        if (source[at] == '/' && at + 1 < source.size() && source[at + 1] == '/') {
          at = source.find('\n', at);
          if (at == std::string_view::npos) return std::string(source);
          continue;
        }
        if (source[at] == '/' && at + 1 < source.size() && source[at + 1] == '*') {
          at = source.find("*/", at + 2);
          if (at == std::string_view::npos) return std::string(source);
          ++at;
          continue;
        }
        if (source[at] == '{') ++depth;
        if (source[at] == '}' && --depth == 0) break;
      }
      if (at >= source.size()) return std::string(source); // not the shape expected: leave it alone
      size_t end = source.find('\n', at);
      end = end == std::string_view::npos ? source.size() : end + 1;
      byName.emplace(source.substr(line + 3, nameEnd - line - 3), functions.size());
      functions.push_back({source.substr(line + 3, nameEnd - line - 3), line, end});
      line = end;
      continue;
    }
    line = lineEnd + 1;
  }
  if (functions.empty()) return std::string(source);

  std::vector<size_t> pending;
  const auto markNames = [&](std::string_view text) {
    for (size_t at = 0; at < text.size();) {
      if (std::isalpha(static_cast<unsigned char>(text[at])) || text[at] == '_') {
        size_t end = at;
        while (end < text.size() && (std::isalnum(static_cast<unsigned char>(text[end])) || text[end] == '_')) ++end;
        const auto found = byName.find(text.substr(at, end - at));
        if (found != byName.end() && !functions[found->second].kept) {
          functions[found->second].kept = true;
          pending.push_back(found->second);
        }
        at = end;
      } else {
        ++at;
      }
    }
  };
  // Roots: everything outside functions, and entry points (an attribute line right before "fn").
  size_t previous = 0;
  for (auto& function : functions) {
    const auto between = source.substr(previous, function.begin - previous);
    markNames(between);
    const auto lastLine = between.substr(between.rfind('\n', between.size() >= 2 ? between.size() - 2 : 0) + 1);
    if (lastLine.find('@') != std::string_view::npos && !function.kept) {
      function.kept = true;
      pending.push_back(static_cast<size_t>(&function - functions.data()));
    }
    previous = function.end;
  }
  markNames(source.substr(previous));
  while (!pending.empty()) {
    const auto& function = functions[pending.back()];
    pending.pop_back();
    // Skip the function's own name so recursion does not keep it.
    const auto body = source.substr(function.begin, function.end - function.begin);
    markNames(body.substr(body.find('(')));
  }

  std::string pruned;
  pruned.reserve(source.size());
  previous = 0;
  for (const auto& function : functions) {
    pruned.append(source.substr(previous, function.begin - previous));
    if (function.kept) pruned.append(source.substr(function.begin, function.end - function.begin));
    previous = function.end;
  }
  pruned.append(source.substr(previous));
  return pruned;
}

} // namespace aurora::gx

#endif
