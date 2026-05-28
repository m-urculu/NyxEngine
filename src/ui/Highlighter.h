#pragma once

// Highlighter.h — tree-sitter-backed syntax highlighting for the code editor.
// Parses a buffer as C/C++ and precomputes a per-line, per-character color array
// from a highlight query (VS Code "Dark+"-style theme). Degrades gracefully:
// ready() == false (grammar/query failed) → the editor falls back to its own
// lexer.

#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <cstdint>

struct TSParser;
struct TSTree;
struct TSQuery;

namespace Nyx {

class Highlighter {
public:
    void init();
    void cleanup();
    bool ready() const { return m_ready; }

    // Re-parse `text` (document lines joined by '\n') and recompute colors.
    void setText(const std::string& text);

    // Fill `out` (size lineLen) with the colors for line `li`; false if none.
    bool lineColors(int li, int lineLen, std::vector<glm::vec4>& out) const;

private:
    TSParser* m_parser = nullptr;
    TSTree*   m_tree   = nullptr;
    TSQuery*  m_query  = nullptr;
    bool      m_ready  = false;

    std::vector<glm::vec4>              m_captureColors;  // by query capture id
    std::vector<std::vector<glm::vec4>> m_lineColors;     // [line][column]
};

} // namespace Nyx
