#include "ui/Highlighter.h"
#include "Logger.h"

#include <tree_sitter/api.h>

#include <algorithm>
#include <fstream>
#include <iterator>

// The generated grammar exports this C symbol.
extern "C" const TSLanguage* tree_sitter_cpp(void);

namespace Nyx {

namespace {
const glm::vec4 kDefault = {0.83f, 0.83f, 0.83f, 1.0f};   // #D4D4D4

// VS Code "Dark+"-inspired theme keyed by tree-sitter highlight capture name.
glm::vec4 colorForCapture(const std::string& name) {
    auto hex = [](int r, int g, int b) { return glm::vec4(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f); };
    if (name == "comment")          return hex(106, 153,  85);   // green
    if (name == "string")           return hex(206, 145, 120);   // orange
    if (name == "number")           return hex(181, 206, 168);   // light green
    if (name == "keyword")          return hex( 86, 156, 214);   // blue
    if (name == "type")             return hex( 78, 201, 176);   // teal
    if (name == "function")         return hex(220, 220, 170);   // yellow
    if (name == "function.special") return hex(197, 134, 192);   // preprocessor (purple)
    if (name == "constant")         return hex( 79, 193, 255);   // bright blue
    if (name == "constant.builtin") return hex( 86, 156, 214);
    if (name == "variable.builtin") return hex( 86, 156, 214);   // this / nullptr
    if (name == "property")         return hex(156, 220, 254);   // light blue
    if (name == "label")            return hex(200, 200, 200);
    if (name == "operator")         return hex(212, 212, 212);
    if (name == "delimiter")        return hex(128, 131, 138);   // dim grey
    if (name == "variable")         return kDefault;
    // Fall back to the prefix before a dot (e.g. "string.escape" → "string").
    size_t dot = name.find('.');
    if (dot != std::string::npos) return colorForCapture(name.substr(0, dot));
    return kDefault;
}
} // namespace

void Highlighter::init() {
    m_parser = ts_parser_new();
    const TSLanguage* lang = tree_sitter_cpp();
    if (!ts_parser_set_language(m_parser, lang)) {
        LOG_WARN("Highlighter: tree-sitter grammar ABI incompatible with runtime");
        return;
    }

    std::ifstream f("external/tree-sitter-cpp/queries/highlights_full.scm", std::ios::binary);
    if (!f.is_open()) { LOG_WARN("Highlighter: highlight query file not found"); return; }
    std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    uint32_t errOff = 0; TSQueryError errType = TSQueryErrorNone;
    m_query = ts_query_new(lang, src.c_str(), (uint32_t)src.size(), &errOff, &errType);
    if (!m_query) {
        LOG_WARN("Highlighter: query compile failed (type {}, byte {})", (int)errType, errOff);
        return;
    }

    uint32_t n = ts_query_capture_count(m_query);
    m_captureColors.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t len = 0;
        const char* nm = ts_query_capture_name_for_id(m_query, i, &len);
        m_captureColors[i] = colorForCapture(std::string(nm, len));
    }
    m_ready = true;
    LOG_INFO("Highlighter: tree-sitter C/C++ ready ({} captures)", n);
}

void Highlighter::cleanup() {
    if (m_query)  { ts_query_delete(m_query);   m_query = nullptr; }
    if (m_tree)   { ts_tree_delete(m_tree);     m_tree = nullptr; }
    if (m_parser) { ts_parser_delete(m_parser); m_parser = nullptr; }
    m_ready = false;
}

void Highlighter::setText(const std::string& text) {
    if (!m_ready) return;
    if (m_tree) { ts_tree_delete(m_tree); m_tree = nullptr; }
    m_tree = ts_parser_parse_string(m_parser, nullptr, text.c_str(), (uint32_t)text.size());

    // Per-line color buffers, sized to each line's length, defaulted.
    m_lineColors.clear();
    {
        size_t start = 0;
        for (;;) {
            size_t nl = text.find('\n', start);
            size_t end = (nl == std::string::npos) ? text.size() : nl;
            m_lineColors.emplace_back(end - start, kDefault);
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
        if (m_lineColors.empty()) m_lineColors.emplace_back();
    }
    if (!m_tree) return;

    // One pass over the highlight query; later captures overwrite earlier ones
    // (so C++ refinements win over the inherited C rules). Columns are byte
    // offsets — our buffer is ASCII (tabs already expanded), so byte == char.
    TSNode root = ts_tree_root_node(m_tree);
    TSQueryCursor* cur = ts_query_cursor_new();
    ts_query_cursor_exec(cur, m_query, root);
    TSQueryMatch match; uint32_t capIdx;
    while (ts_query_cursor_next_capture(cur, &match, &capIdx)) {
        const TSQueryCapture& cap = match.captures[capIdx];
        if (cap.index >= m_captureColors.size()) continue;
        const glm::vec4& col = m_captureColors[cap.index];
        TSPoint s = ts_node_start_point(cap.node);
        TSPoint e = ts_node_end_point(cap.node);
        for (uint32_t row = s.row; row <= e.row && row < m_lineColors.size(); ++row) {
            auto& line = m_lineColors[row];
            uint32_t c0 = (row == s.row) ? s.column : 0u;
            uint32_t c1 = (row == e.row) ? e.column : (uint32_t)line.size();
            for (uint32_t c = c0; c < c1 && c < line.size(); ++c) line[c] = col;
        }
    }
    ts_query_cursor_delete(cur);
}

bool Highlighter::lineColors(int li, int lineLen, std::vector<glm::vec4>& out) const {
    if (!m_ready || li < 0 || li >= (int)m_lineColors.size()) return false;
    const auto& src = m_lineColors[li];
    out.assign(lineLen, kDefault);
    int n = std::min(lineLen, (int)src.size());
    for (int i = 0; i < n; ++i) out[i] = src[i];
    return true;
}

} // namespace Nyx
