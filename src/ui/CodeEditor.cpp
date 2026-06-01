#include "ui/CodeEditor.h"
#include "ui/PixelFont.h"
#include "renderer/VulkanContext.h"
#include "Logger.h"

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <filesystem>
#include <set>
#include <sstream>
#include <cctype>

namespace fs = std::filesystem;

namespace Nyx {

namespace {
constexpr float CHAR_W  = static_cast<float>(PixelFont::ADVANCE);      // 6 — normal glyph advance
constexpr float SPACE_W = 3.0f;                                        // narrower advance for whitespace
constexpr float LINE_H  = static_cast<float>(PixelFont::CELL_H) + 3.0f; // 10
constexpr size_t MAX_TAB_CHARS = 26;

// Per-character advance: spaces/tabs are thinner than glyphs, so indentation
// doesn't spray code across the screen. (Tabs are expanded to spaces on load.)
inline float advanceOf(char c) { return (c == ' ' || c == '\t') ? SPACE_W : CHAR_W; }

// Pixel width of columns [from, to) on a line, honoring the narrower whitespace.
float colToX(const std::string& line, int from, int to) {
    float x = 0.0f;
    for (int c = from; c < to; ++c)
        x += (c < (int)line.size()) ? advanceOf(line[c]) : CHAR_W;
    return x;
}
} // namespace

std::string CodeEditor::tabLabel(const Doc& d) const {
    if (d.name.size() <= MAX_TAB_CHARS) return d.name;
    size_t dot = d.name.find_last_of('.');
    std::string ext = (dot != std::string::npos) ? d.name.substr(dot) : "";
    if (ext.size() + 3 >= MAX_TAB_CHARS) return d.name.substr(0, MAX_TAB_CHARS);
    size_t keep = MAX_TAB_CHARS - ext.size() - 2;   // 2 for ".."
    return d.name.substr(0, keep) + ".." + ext;     // keep prefix + extension
}

float CodeEditor::tabW(const Doc& d) const {
    // Snug: label (+ '*' only when modified) + the close 'x' + small margins.
    int chars = static_cast<int>(tabLabel(d).size()) + (d.modified ? 1 : 0);
    return chars * CHAR_W + 15.0f;
}

// ─── Lifecycle ───────────────────────────────────────────────────────────────

void CodeEditor::init(VulkanContext& context, GLFWwindow* window,
                      ImagePipeline* imgPipeline, MaterialPreviewPipeline* matPipeline) {
    m_context     = &context;
    m_device      = context.getDevice();
    m_allocator   = context.getAllocator();
    m_window      = window;
    m_imgPipeline = imgPipeline;
    m_matPipeline = matPipeline;
    m_docs.reserve(64);

    m_vertexBuffer.init(m_allocator, sizeof(UIVertex) * VERT_CAP,
                        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
    m_indexBuffer.init(m_allocator, sizeof(uint32_t) * VERT_CAP * 3 / 2,
                       VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
    m_imgVB.init(m_allocator, sizeof(ImageVertex) * 4, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
    m_imgIB.init(m_allocator, sizeof(uint32_t) * 6, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
    m_buffersInitialized = true;
    buildSphere();

    // Descriptor pool for asset tabs (combined image sampler, freeable per tab).
    VkDescriptorPoolSize ps{}; ps.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; ps.descriptorCount = 64;
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pi.poolSizeCount = 1; pi.pPoolSizes = &ps; pi.maxSets = 64;
    if (vkCreateDescriptorPool(m_device, &pi, nullptr, &m_descPool) != VK_SUCCESS)
        throw std::runtime_error("CodeEditor: failed to create descriptor pool");
    m_hl.init();
}

void CodeEditor::cleanup(VmaAllocator allocator) {
    for (auto& d : m_docs) freeDocAsset(d);
    if (m_descPool) { vkDestroyDescriptorPool(m_device, m_descPool, nullptr); m_descPool = VK_NULL_HANDLE; }
    if (m_buffersInitialized) {
        m_vertexBuffer.cleanup(allocator); m_indexBuffer.cleanup(allocator);
        m_imgVB.cleanup(allocator); m_imgIB.cleanup(allocator);
        m_sphereVB.cleanup(allocator); m_sphereIB.cleanup(allocator);
        m_buffersInitialized = false;
    }
    m_hl.cleanup();
}

// ─── Documents ───────────────────────────────────────────────────────────────

void CodeEditor::openFile(const std::string& path) {
    m_showScene = false;   // opening a file reveals the editor
    for (int i = 0; i < (int)m_docs.size(); ++i) {
        if (m_docs[i].path == path) { m_active = i; m_focused = true; m_dirty = true; return; }
    }
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) { LOG_WARN("CodeEditor: cannot open {}", path); return; }

    Doc d;
    d.path = path;
    d.name = std::filesystem::path(path).filename().string();
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::string cur;
    for (char ch : content) {
        if      (ch == '\n') { d.lines.push_back(cur); cur.clear(); }
        else if (ch == '\r') { /* skip CR */ }
        else if (ch == '\t') { cur += "    "; }
        else                  { cur += ch; }
    }
    d.lines.push_back(cur);
    if (d.lines.empty()) d.lines.push_back("");

    m_docs.push_back(std::move(d));
    m_active  = (int)m_docs.size() - 1;
    m_focused = true;
    m_dirty   = true;
    LOG_INFO("CodeEditor: opened {} ({} lines)", path, m_docs[m_active].lines.size());
}

void CodeEditor::saveDoc(Doc& d) {
    if (d.kind != Kind::Text) return;     // assets are read-only previews
    std::ofstream f(d.path, std::ios::binary);
    if (!f.is_open()) { LOG_ERROR("CodeEditor: cannot save {}", d.path); return; }
    for (size_t i = 0; i < d.lines.size(); ++i) {
        f << d.lines[i];
        if (i + 1 < d.lines.size()) f << "\n";
    }
    d.modified = false;
    m_dirty = true;
    LOG_INFO("CodeEditor: saved {}", d.path);
}

void CodeEditor::save() { if (Doc* d = active()) saveDoc(*d); }

void CodeEditor::saveAll() {
    for (auto& d : m_docs) if (d.kind == Kind::Text && d.modified) saveDoc(d);
}

void CodeEditor::closeTab(int i) {
    if (i < 0 || i >= (int)m_docs.size()) return;
    if (m_docs[i].hasTex || m_docs[i].descSet) { vkDeviceWaitIdle(m_device); freeDocAsset(m_docs[i]); }
    m_docs.erase(m_docs.begin() + i);
    if (m_docs.empty())      m_active = -1;
    else if (m_active >= (int)m_docs.size()) m_active = (int)m_docs.size() - 1;
    else if (i < m_active)   m_active--;
    m_dirty = true;
}

// ─── Asset tabs (image / material / binary previews) ─────────────────────────

void CodeEditor::freeDocAsset(Doc& d) {
    if (d.hasTex)  { d.texture.cleanup(m_device, m_allocator); d.hasTex = false; }
    if (d.descSet) { vkFreeDescriptorSets(m_device, m_descPool, 1, &d.descSet); d.descSet = VK_NULL_HANDLE; }
}

std::string CodeEditor::humanSize(uint64_t b) {
    char buf[32];
    if (b < 1024)            std::snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)b);
    else if (b < 1024*1024)  std::snprintf(buf, sizeof(buf), "%.1f KB", b / 1024.0);
    else                     std::snprintf(buf, sizeof(buf), "%.1f MB", b / (1024.0*1024.0));
    return buf;
}

void CodeEditor::buildSphere() {
    const int STACKS = 24, SLICES = 32;
    const float PI = 3.14159265358979f;
    std::vector<PreviewVertex> v; std::vector<uint32_t> idx;
    for (int i = 0; i <= STACKS; ++i) {
        float phi = PI * i / STACKS; float y = std::cos(phi), r = std::sin(phi);
        for (int j = 0; j <= SLICES; ++j) {
            float th = 2.0f * PI * j / SLICES;
            glm::vec3 p = { r * std::cos(th), y, r * std::sin(th) };
            v.push_back({ p, p, { (float)j / SLICES, (float)i / STACKS } });
        }
    }
    for (int i = 0; i < STACKS; ++i)
        for (int j = 0; j < SLICES; ++j) {
            uint32_t a = i * (SLICES + 1) + j, b = a + (SLICES + 1);
            idx.push_back(a); idx.push_back(b); idx.push_back(a + 1);
            idx.push_back(a + 1); idx.push_back(b); idx.push_back(b + 1);
        }
    m_sphereVB.init(m_allocator, v.size() * sizeof(PreviewVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
    m_sphereIB.init(m_allocator, idx.size() * sizeof(uint32_t),    VK_BUFFER_USAGE_INDEX_BUFFER_BIT,  VMA_MEMORY_USAGE_CPU_TO_GPU);
    m_sphereVB.uploadData(m_allocator, v.data(),   v.size() * sizeof(PreviewVertex));
    m_sphereIB.uploadData(m_allocator, idx.data(), idx.size() * sizeof(uint32_t));
    m_sphereIndexCount = (uint32_t)idx.size();
}

bool CodeEditor::loadDocTexture(Doc& d, const std::string& imgPath) {
    bool loaded = false;
    if (!imgPath.empty()) {
        try { d.texture.loadFromFile(*m_context, imgPath); loaded = true; } catch (...) { loaded = false; }
    }
    if (!loaded) {
        if (d.kind == Kind::Material) d.texture.createDefault(*m_context);   // white fallback
        else return false;                                                    // image must decode
    }
    d.imgW = d.texture.getWidth(); d.imgH = d.texture.getHeight();
    VkDescriptorSetLayout layout = (d.kind == Kind::Material)
                                 ? m_matPipeline->getDescriptorSetLayout()
                                 : m_imgPipeline->getDescriptorSetLayout();
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = m_descPool; ai.descriptorSetCount = 1; ai.pSetLayouts = &layout;
    if (vkAllocateDescriptorSets(m_device, &ai, &d.descSet) != VK_SUCCESS) return false;
    VkDescriptorImageInfo ii{};
    ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ii.imageView = d.texture.getImageView(); ii.sampler = d.texture.getSampler();
    VkWriteDescriptorSet w{};
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = d.descSet; w.dstBinding = 0;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.descriptorCount = 1; w.pImageInfo = &ii;
    vkUpdateDescriptorSets(m_device, 1, &w, 0, nullptr);
    d.hasTex = true;
    return true;
}

void CodeEditor::openImage(const std::string& path) {
    m_showScene = false;
    for (int i = 0; i < (int)m_docs.size(); ++i)
        if (m_docs[i].path == path) { m_active = i; m_dirty = true; return; }
    vkDeviceWaitIdle(m_device);
    m_docs.push_back(Doc{});
    Doc& d = m_docs.back();
    d.kind = Kind::Image; d.path = path; d.name = fs::path(path).filename().string();
    std::error_code ec; d.fileSize = fs::file_size(path, ec);
    if (!loadDocTexture(d, path)) d.kind = Kind::Binary;   // not a decodable image
    m_active = (int)m_docs.size() - 1; m_dirty = true;
}

void CodeEditor::openMaterial(const std::string& path) {
    m_showScene = false;
    for (int i = 0; i < (int)m_docs.size(); ++i)
        if (m_docs[i].path == path) { m_active = i; m_dirty = true; return; }
    vkDeviceWaitIdle(m_device);
    m_docs.push_back(Doc{});
    Doc& d = m_docs.back();
    d.kind = Kind::Material; d.path = path; d.name = fs::path(path).filename().string();
    std::error_code ec; d.fileSize = fs::file_size(path, ec);
    d.baseColor = {0.80f, 0.80f, 0.85f, 1.0f}; d.metallic = 0.0f; d.roughness = 0.5f;
    std::string albedoPath;
    std::ifstream f(path); std::string line;
    while (std::getline(f, line)) {
        std::istringstream ss(line); std::string key; ss >> key;
        if      (key == "baseColor") ss >> d.baseColor.r >> d.baseColor.g >> d.baseColor.b >> d.baseColor.a;
        else if (key == "metallic")  ss >> d.metallic;
        else if (key == "roughness") ss >> d.roughness;
        else if (key == "albedo")    { std::getline(ss, albedoPath);
                                       size_t s = albedoPath.find_first_not_of(" \t");
                                       albedoPath = (s == std::string::npos) ? "" : albedoPath.substr(s); }
    }
    if (!albedoPath.empty()) d.albedoName = fs::path(albedoPath).filename().string();
    loadDocTexture(d, albedoPath);
    m_active = (int)m_docs.size() - 1; m_dirty = true;
}

void CodeEditor::openBinary(const std::string& path) {
    m_showScene = false;
    for (int i = 0; i < (int)m_docs.size(); ++i)
        if (m_docs[i].path == path) { m_active = i; m_dirty = true; return; }
    m_docs.push_back(Doc{});
    Doc& d = m_docs.back();
    d.kind = Kind::Binary; d.path = path; d.name = fs::path(path).filename().string();
    std::error_code ec; d.fileSize = fs::file_size(path, ec);
    m_active = (int)m_docs.size() - 1; m_dirty = true;
}

bool CodeEditor::activeIsImage() const {
    const Doc* d = active(); return isVisible() && !m_showScene && d && d->kind == Kind::Image && d->hasTex;
}
bool CodeEditor::activeIsMaterial() const {
    const Doc* d = active(); return isVisible() && !m_showScene && d && d->kind == Kind::Material && d->hasTex;
}

void CodeEditor::addImageQuad(float x, float y, float w, float h) {
    uint32_t b = (uint32_t)m_imgVerts.size();
    m_imgVerts.push_back({{x,y},{0,0}});
    m_imgVerts.push_back({{x+w,y},{1,0}});
    m_imgVerts.push_back({{x+w,y+h},{1,1}});
    m_imgVerts.push_back({{x,y+h},{0,1}});
    m_imgIdx.push_back(b); m_imgIdx.push_back(b+1); m_imgIdx.push_back(b+2);
    m_imgIdx.push_back(b+2); m_imgIdx.push_back(b+3); m_imgIdx.push_back(b);
}

// Close any tab whose file was removed — the exact path, or anything under it
// when a folder is deleted.
void CodeEditor::closePath(const std::string& path) {
    if (path.empty()) return;
    std::string prefix = path + "/";
    for (int i = (int)m_docs.size() - 1; i >= 0; --i) {
        const std::string& p = m_docs[i].path;
        if (p == path || p.rfind(prefix, 0) == 0) closeTab(i);
    }
}

void CodeEditor::clampCursor(Doc& d) {
    if (d.cline < 0) d.cline = 0;
    if (d.cline >= (int)d.lines.size()) d.cline = (int)d.lines.size() - 1;
    if (d.ccol < 0) d.ccol = 0;
    if (d.ccol > (int)d.lines[d.cline].size()) d.ccol = (int)d.lines[d.cline].size();
}

void CodeEditor::ensureCursorVisible() {
    Doc* d = active();
    if (!d) return;
    if (d->cline < d->scrollLine) d->scrollLine = d->cline;
    if (m_visRows > 0 && d->cline >= d->scrollLine + m_visRows) d->scrollLine = d->cline - m_visRows + 1;
    if (d->ccol < d->scrollCol) d->scrollCol = d->ccol;
    if (m_visCols > 0 && d->ccol >= d->scrollCol + m_visCols) d->scrollCol = d->ccol - m_visCols + 1;
    if (d->scrollLine < 0) d->scrollLine = 0;
    if (d->scrollCol  < 0) d->scrollCol  = 0;
}

// ─── Input ───────────────────────────────────────────────────────────────────

void CodeEditor::handleChar(unsigned int cp) {
    if (!isFocused()) return;
    Doc* d = active();
    if (!d || cp < 32 || cp > 126) return;
    pushUndo(*d, true);
    if (hasSel(*d)) deleteSel(*d);
    d->lines[d->cline].insert(d->ccol, 1, static_cast<char>(cp));
    d->ccol++;
    d->sel = false;
    d->modified = true;
    ensureCursorVisible();
    m_dirty = true;
}

void CodeEditor::handleKey(int key, int action, int mods) {
    if (!isFocused()) return;
    if (action != GLFW_PRESS && action != GLFW_REPEAT) return;
    Doc* d = active();
    if (!d) return;
    bool ctrl  = (mods & GLFW_MOD_CONTROL) != 0;
    bool shift = (mods & GLFW_MOD_SHIFT)   != 0;

    if (ctrl) {
        switch (key) {
            case GLFW_KEY_S: save();             break;
            case GLFW_KEY_C: copySel(false);     break;
            case GLFW_KEY_X: copySel(true);      break;
            case GLFW_KEY_V: paste();            break;
            case GLFW_KEY_Z: shift ? doRedo() : doUndo(); break;
            case GLFW_KEY_Y: doRedo();           break;
            case GLFW_KEY_A: selectAll();        break;
            default: break;
        }
        return;   // never fall through to typing for ctrl combos
    }

    const bool isMove = (key == GLFW_KEY_LEFT || key == GLFW_KEY_RIGHT || key == GLFW_KEY_UP ||
                         key == GLFW_KEY_DOWN || key == GLFW_KEY_HOME || key == GLFW_KEY_END);
    if (isMove) {
        if (shift) { if (!d->sel) { d->sel = true; d->aline = d->cline; d->acol = d->ccol; } }
        else d->sel = false;
    }

    switch (key) {
        case GLFW_KEY_BACKSPACE:
            pushUndo(*d, false);
            if (hasSel(*d)) deleteSel(*d);
            else if (d->ccol > 0) { d->lines[d->cline].erase(d->ccol - 1, 1); d->ccol--; }
            else if (d->cline > 0) {
                int prev = (int)d->lines[d->cline - 1].size();
                d->lines[d->cline - 1] += d->lines[d->cline];
                d->lines.erase(d->lines.begin() + d->cline);
                d->cline--; d->ccol = prev;
            }
            d->sel = false; d->modified = true;
            break;
        case GLFW_KEY_DELETE:
            pushUndo(*d, false);
            if (hasSel(*d)) deleteSel(*d);
            else {
                std::string& l = d->lines[d->cline];
                if (d->ccol < (int)l.size()) l.erase(d->ccol, 1);
                else if (d->cline + 1 < (int)d->lines.size()) {
                    l += d->lines[d->cline + 1];
                    d->lines.erase(d->lines.begin() + d->cline + 1);
                }
            }
            d->sel = false; d->modified = true;
            break;
        case GLFW_KEY_ENTER:
        case GLFW_KEY_KP_ENTER: {
            pushUndo(*d, false);
            if (hasSel(*d)) deleteSel(*d);
            std::string& l = d->lines[d->cline];
            std::string rest = l.substr(d->ccol);
            l.erase(d->ccol);
            d->lines.insert(d->lines.begin() + d->cline + 1, rest);
            d->cline++; d->ccol = 0; d->sel = false; d->modified = true;
            break;
        }
        case GLFW_KEY_TAB:
            pushUndo(*d, false);
            if (hasSel(*d)) deleteSel(*d);
            d->lines[d->cline].insert(d->ccol, "    ");
            d->ccol += 4; d->sel = false; d->modified = true;
            break;
        case GLFW_KEY_LEFT:
            if (d->ccol > 0) d->ccol--;
            else if (d->cline > 0) { d->cline--; d->ccol = (int)d->lines[d->cline].size(); }
            break;
        case GLFW_KEY_RIGHT:
            if (d->ccol < (int)d->lines[d->cline].size()) d->ccol++;
            else if (d->cline + 1 < (int)d->lines.size()) { d->cline++; d->ccol = 0; }
            break;
        case GLFW_KEY_UP:
            if (d->cline > 0) { d->cline--; d->ccol = std::min(d->ccol, (int)d->lines[d->cline].size()); }
            break;
        case GLFW_KEY_DOWN:
            if (d->cline + 1 < (int)d->lines.size()) { d->cline++; d->ccol = std::min(d->ccol, (int)d->lines[d->cline].size()); }
            break;
        case GLFW_KEY_HOME: d->ccol = 0; break;
        case GLFW_KEY_END:  d->ccol = (int)d->lines[d->cline].size(); break;
        default: return;
    }
    if (!isMove) d->coalesce = false;
    clampCursor(*d);
    ensureCursorVisible();
    m_dirty = true;
}

bool CodeEditor::handleMouseButton(int button, int action, int mods) {
    if (!isVisible()) return false;
    if (button != GLFW_MOUSE_BUTTON_LEFT || action != GLFW_PRESS) return false;
    double mx = 0.0, my = 0.0;
    glfwGetCursorPos(m_window, &mx, &my);
    if (mx < m_x || mx >= m_x + m_w || my < m_y || my >= m_y + m_h) return false;

    if (my < m_y + TABBAR_H) {                          // tab bar
        m_dirty = true;
        // View toggle (right edge) — flip between editing and the 3D scene view.
        if (mx >= m_x + m_w - VIEW_BTN_W) {
            m_showScene = !m_showScene;
            if (m_showScene) m_focused = false;          // release focus so camera WASD works
            return true;
        }
        // A tab click always returns to the editor view, then selects/closes.
        m_showScene = false;
        m_focused = true;
        float tx = m_x;
        for (int i = 0; i < (int)m_docs.size(); ++i) {
            float tw = tabW(m_docs[i]);
            if (mx >= tx && mx < tx + tw) {
                if (mx >= tx + tw - 11.0f) closeTab(i);  // close 'x'
                else                       m_active = i;
                break;
            }
            tx += tw;
        }
        return true;
    }

    // Body shows the 3D scene → let the click reach the viewport (pick / gizmo).
    if (m_showScene) return false;

    m_focused = true;
    m_dirty = true;

    Doc* d = active();                                  // text area → place cursor
    if (d && d->kind == Kind::Text) {
        bool shift = (mods & GLFW_MOD_SHIFT) != 0;
        if (shift) { if (!d->sel) { d->sel = true; d->aline = d->cline; d->acol = d->ccol; } }
        else d->sel = false;
        int row  = static_cast<int>(std::floor((my - m_textTop) / LINE_H));
        int line = d->scrollLine + row;
        if (line < 0) line = 0;
        if (line >= (int)d->lines.size()) line = (int)d->lines.size() - 1;
        // Walk variable-width columns from the scroll origin to the click x.
        const std::string& cl = d->lines[line];
        float target = static_cast<float>(mx) - m_textX;
        float acc = 0.0f;
        int col = d->scrollCol;
        while (col < (int)cl.size()) {
            float w = advanceOf(cl[col]);
            if (acc + w * 0.5f >= target) break;   // snap to the nearest boundary
            acc += w; col++;
        }
        d->cline = line;
        d->ccol  = std::clamp(col, 0, (int)cl.size());
    }
    return true;
}

bool CodeEditor::handleScroll(double yoffset) {
    if (!isVisible() || m_showScene) return false;   // scene view → wheel zooms the camera
    double mx = 0.0, my = 0.0;
    glfwGetCursorPos(m_window, &mx, &my);
    if (mx < m_x || mx >= m_x + m_w || my < m_y + TABBAR_H || my >= m_y + m_h) return false;
    Doc* d = active();
    if (!d) return false;
    if (d->kind != Kind::Text) return true;     // assets don't scroll, but consume the wheel here
    d->scrollLine -= static_cast<int>(std::lround(yoffset)) * 3;
    int maxScroll = std::max(0, (int)d->lines.size() - 1);
    d->scrollLine = std::clamp(d->scrollLine, 0, maxScroll);
    m_dirty = true;
    return true;
}

// ─── Update / render ─────────────────────────────────────────────────────────

void CodeEditor::update(float left, float top, float right, float bottom) {
    m_x = left; m_y = top; m_w = right - left; m_h = bottom - top;
    // Tab close 'x' hover — re-tested every frame (not gated by m_dirty) so
    // the engine flips to the hand cursor as soon as the cursor crosses it.
    m_overButton = false;
    if (isVisible() && m_w >= 1.0f && m_h >= TABBAR_H) {
        double cmx = 0.0, cmy = 0.0;
        glfwGetCursorPos(m_window, &cmx, &cmy);
        if (cmx >= m_x && cmx < m_x + m_w && cmy >= m_y && cmy < m_y + TABBAR_H) {
            if (cmx >= m_x + m_w - VIEW_BTN_W) {
                m_overButton = true;                     // over the editor⇄scene toggle
            } else {
                float tx = m_x;
                for (const Doc& d : m_docs) {
                    float tw = tabW(d);
                    if (cmx >= tx + tw - 11.0f && cmx < tx + tw) { m_overButton = true; break; }
                    tx += tw;
                }
            }
        }
    }
    if (!isVisible() || m_w < 1.0f || m_h < TABBAR_H) { m_indexCount = 0; return; }

    Doc* d = active();
    int lineCount = d ? (int)d->lines.size() : 1;
    int digits = 1; for (int n = lineCount; n >= 10; n /= 10) digits++;
    m_gutterW  = (digits + 1) * CHAR_W + 8.0f;
    m_textX    = m_x + m_gutterW;
    m_textTop  = m_y + TABBAR_H + 2.0f;
    m_visRows  = std::max(0, static_cast<int>(std::floor((m_y + m_h - m_textTop) / LINE_H)));
    m_visCols  = std::max(0, static_cast<int>(std::floor((m_x + m_w - m_textX) / CHAR_W)));

    if (!m_dirty && m_x == m_lastX && m_y == m_lastY && m_w == m_lastW && m_h == m_lastH) return;
    m_lastX = m_x; m_lastY = m_y; m_lastW = m_w; m_lastH = m_h;
    m_dirty = false;

    m_vertices.clear();
    m_indices.clear();
    m_imgVerts.clear(); m_imgIdx.clear(); m_imgIndexCount = 0;
    m_sphereVp = glm::vec4(0.0f);

    const glm::vec4 bg        = {0.08f, 0.085f, 0.10f, 1.0f};
    const glm::vec4 tabbarBg  = {0.05f, 0.055f, 0.07f, 1.0f};
    const glm::vec4 tabActive = {0.10f, 0.11f,  0.14f, 1.0f};
    const glm::vec4 tabInact  = {0.055f,0.06f,  0.075f,1.0f};
    const glm::vec4 accent    = {0.42f, 1.00f,  0.66f, 1.0f};
    const glm::vec4 tabText   = {0.85f, 0.87f,  0.92f, 1.0f};
    const glm::vec4 dimText   = {0.55f, 0.58f,  0.64f, 1.0f};
    const glm::vec4 gutterBg  = {0.06f, 0.065f, 0.085f,1.0f};
    const glm::vec4 lineNum   = {0.42f, 0.45f,  0.52f, 1.0f};
    const glm::vec4 selBg     = {0.18f, 0.34f,  0.50f, 1.0f};
    const glm::vec4 caretCol  = {0.42f, 1.00f,  0.66f, 1.0f};

    const float labelY = m_y + std::floor((TABBAR_H - PixelFont::CELL_H) * 0.5f);

    // Editor background + tab bar. In scene view the body background is skipped
    // so the 3D viewport shows through below the tab bar.
    if (!m_showScene) addQuad(m_x, m_y, m_w, m_h, bg);
    addQuad(m_x, m_y, m_w, TABBAR_H, tabbarBg);

    // Tabs.
    float tx = m_x;
    for (int i = 0; i < (int)m_docs.size(); ++i) {
        float tw = tabW(m_docs[i]);
        bool act = (i == m_active);
        addQuad(tx, m_y, tw, TABBAR_H, act ? tabActive : tabInact);
        if (act) addQuad(tx, m_y + TABBAR_H - 2.0f, tw, 2.0f, accent);
        std::string label = (m_docs[i].modified ? "*" : "") + tabLabel(m_docs[i]);
        addText(tx + 4.0f, labelY, label, act ? tabText : dimText, tx + tw - 11.0f);
        addGlyph(tx + tw - 9.0f, labelY, 'x', dimText);
        tx += tw;
    }

    // View toggle (right of the tab bar): switch between the code editor and the
    // 3D scene/game view. Drawn last so it sits over any tab that overflows under
    // it; highlighted while scene view is active.
    {
        float bx = m_x + m_w - VIEW_BTN_W;
        addQuad(bx, m_y, VIEW_BTN_W, TABBAR_H, m_showScene ? tabActive : tabbarBg);
        if (m_showScene) addQuad(bx, m_y + TABBAR_H - 2.0f, VIEW_BTN_W, 2.0f, accent);  // active underline

        // Monitor glyph (screen outline + stand) — reads as "the 3D viewport".
        const glm::vec4 icol = m_showScene ? tabText : dimText;
        float cx = std::floor(bx + VIEW_BTN_W * 0.5f);
        float cy = std::floor(m_y + TABBAR_H * 0.5f);
        float sw = 13.0f, sh = 9.0f;
        float sx = cx - std::floor(sw * 0.5f), sy = cy - std::floor(sh * 0.5f) - 1.0f;
        if (m_showScene) addQuad(sx + 1.0f, sy + 1.0f, sw - 2.0f, sh - 2.0f, accent);  // lit screen
        addQuad(sx,            sy,             sw,   1.0f, icol);   // top
        addQuad(sx,            sy + sh - 1.0f, sw,   1.0f, icol);   // bottom
        addQuad(sx,            sy,             1.0f, sh,   icol);   // left
        addQuad(sx + sw - 1.0f,sy,             1.0f, sh,   icol);   // right
        addQuad(cx - 3.0f,     sy + sh + 1.0f, 6.0f, 1.0f, icol);   // stand base
    }

    // Scene view: nothing more to draw — the body stays empty so the 3D
    // viewport behind this panel is visible. The tab bar above remains live.
    if (m_showScene) { upload(); return; }

    // Asset preview body (image / material / binary) for non-text tabs.
    if (d && d->kind != Kind::Text) {
        const glm::vec4 infoCol = {0.55f, 0.60f, 0.66f, 1.0f};
        const glm::vec4 nameCol = {0.42f, 1.00f, 0.66f, 1.0f};
        const glm::vec4 lab     = {0.50f, 0.54f, 0.60f, 1.0f};
        const glm::vec4 frame   = {0.22f, 0.24f, 0.30f, 1.0f};
        const glm::vec4 checker = {0.16f, 0.17f, 0.21f, 1.0f};
        const float cy0 = m_y + TABBAR_H, ch0 = m_h - TABBAR_H, CWf = CHAR_W;

        // Info line under the tab bar (type / dims / size).
        std::string ext = fs::path(d->name).extension().string();
        if (!ext.empty() && ext[0] == '.') ext = ext.substr(1);
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return (char)std::toupper(c); });
        std::string info = ext;
        if (d->kind == Kind::Image) info += "  " + std::to_string(d->imgW) + "x" + std::to_string(d->imgH);
        info += "  " + humanSize(d->fileSize);
        addText(m_x + 8.0f, cy0 + 6.0f, info, infoCol, m_x + m_w - 4.0f);

        if (d->kind == Kind::Image && d->hasTex && d->imgW > 0 && d->imgH > 0) {
            const float pad = 24.0f;
            float bx = m_x + pad, by = cy0 + 22.0f, bw = m_w - 2*pad, bh = ch0 - 22.0f - pad;
            if (bw > 4 && bh > 4) {
                float aspect = (float)d->imgW / (float)d->imgH, dw, dh;
                if (bw / bh > aspect) { dh = bh; dw = dh * aspect; } else { dw = bw; dh = dw / aspect; }
                float ix = bx + (bw - dw) * 0.5f, iy = by + (bh - dh) * 0.5f;
                addQuad(ix - 1, iy - 1, dw + 2, dh + 2, frame);
                addQuad(ix, iy, dw, dh, checker);
                addImageQuad(ix, iy, dw, dh);
            }
        } else if (d->kind == Kind::Material) {
            addText(m_x + 8.0f, cy0 + 22.0f, "MATERIAL", lab, m_x + m_w);
            float py = cy0 + 40.0f;
            auto rowKV = [&](const std::string& k, const std::string& v) {
                addText(m_x + 12.0f, py, k, infoCol, m_x + m_w);
                addText(m_x + 12.0f + 11 * CWf, py, v, nameCol, m_x + m_w);
                py += 14.0f;
            };
            addText(m_x + 12.0f, py, "base color", infoCol, m_x + m_w);
            addQuad(m_x + 12.0f + 11 * CWf, py - 1.0f, 12.0f, PixelFont::CELL_H + 2.0f, d->baseColor);
            py += 14.0f;
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.2f", d->metallic);  rowKV("metallic", buf);
            std::snprintf(buf, sizeof(buf), "%.2f", d->roughness); rowKV("roughness", buf);
            rowKV("albedo", d->albedoName.empty() ? "(none)" : d->albedoName);

            float regionX = m_x + m_w * 0.40f, regionW = m_w - (m_w * 0.40f);
            float S = std::min(regionW * 0.85f, ch0 * 0.82f);
            if (S > 16.0f) m_sphereVp = { regionX + (regionW - S) * 0.5f, cy0 + (ch0 - S) * 0.5f, S, S };
        } else {  // Binary
            std::string big = ext.empty() ? "FILE" : ext;
            float bxc = m_x + m_w * 0.5f, byc = cy0 + ch0 * 0.5f, bwc = big.size() * CWf;
            addQuad(bxc - bwc*0.5f - 10.0f, byc - 26.0f, bwc + 20.0f, 20.0f, glm::vec4{0.12f,0.125f,0.15f,1.0f});
            addText(bxc - bwc*0.5f, byc - 23.0f, big, nameCol, m_x + m_w);
            std::string msg = "Binary file - no preview";
            addText(bxc - (msg.size()*CWf)*0.5f, byc + 2.0f, msg, infoCol, m_x + m_w);
        }

        if (!m_imgVerts.empty()) {
            m_imgVB.uploadData(m_allocator, m_imgVerts.data(), m_imgVerts.size() * sizeof(ImageVertex));
            m_imgIB.uploadData(m_allocator, m_imgIdx.data(), m_imgIdx.size() * sizeof(uint32_t));
            m_imgIndexCount = (uint32_t)m_imgIdx.size();
        }
        upload();
        return;
    }

    // Text body.
    if (d) {
        float textBottom = m_y + m_h;
        addQuad(m_x, m_textTop - 2.0f, m_gutterW, textBottom - (m_textTop - 2.0f), gutterBg);

        int sl = 0, sc = 0, el = 0, ec = 0;
        bool sel = hasSel(*d);
        if (sel) orderedSel(*d, sl, sc, el, ec);
        const float maxx = m_x + m_w - 2.0f;
        std::vector<glm::vec4> cols;

        // Primary highlighter: tree-sitter. Re-parse the active document whenever
        // its text (or identity) changes; the parse drives per-line colors.
        if (m_hl.ready()) {
            std::string joined;
            size_t total = 0; for (const auto& ln : d->lines) total += ln.size() + 1;
            joined.reserve(total);
            for (size_t i = 0; i < d->lines.size(); ++i) {
                joined += d->lines[i];
                if (i + 1 < d->lines.size()) joined += '\n';
            }
            if (m_active != m_hlDoc || joined != m_hlText) {
                m_hl.setText(joined);
                m_hlText = std::move(joined);
                m_hlDoc  = m_active;
            }
        }

        // Fallback lexer state (only used when tree-sitter is unavailable): bracket
        // depth + block-comment must accumulate from the top of the document, so
        // walk the lines above the viewport first.
        int depth = 0; bool inBlock = false;
        if (!m_hl.ready()) {
            std::vector<glm::vec4> skip;
            for (int li = 0; li < d->scrollLine && li < (int)d->lines.size(); ++li)
                colorizeLine(d->lines[li], skip, depth, inBlock);
        }

        for (int r = 0; r < m_visRows; ++r) {
            int li = d->scrollLine + r;
            if (li >= (int)d->lines.size()) break;
            float ly = m_textTop + r * LINE_H;
            const std::string& s = d->lines[li];

            std::string num = std::to_string(li + 1);
            float numX = m_x + m_gutterW - 6.0f - num.size() * CHAR_W;
            addText(numX, ly, num, lineNum, m_x + m_gutterW - 2.0f);

            // Selection highlight (drawn under the text) — variable-width aware.
            if (sel && li >= sl && li <= el) {
                int cs = (li == sl) ? sc : 0;
                int ce = (li == el) ? ec : (int)s.size();
                int vcs = std::max(cs, d->scrollCol);
                float hx = m_textX + colToX(s, d->scrollCol, vcs);
                float hw = colToX(s, vcs, ce);
                if (li < el && ce == (int)s.size()) hw += SPACE_W;  // hint trailing newline
                if (hx + hw > maxx) hw = maxx - hx;
                if (hw > 0.5f) addQuad(hx, ly - 1.0f, hw, PixelFont::CELL_H + 2.0f, selBg);
            }

            // Syntax-colored text: tree-sitter per-line colors, else the fallback lexer.
            if (!(m_hl.ready() && m_hl.lineColors(li, (int)s.size(), cols)))
                colorizeLine(s, cols, depth, inBlock);
            float cx = m_textX;
            for (int c = d->scrollCol; c < (int)s.size(); ++c) {
                if (cx + PixelFont::CELL_W > maxx) break;
                addGlyph(cx, ly, s[c], cols[c]);
                cx += advanceOf(s[c]);           // spaces/tabs advance less
            }
        }

        // Caret.
        if (m_focused &&
            d->cline >= d->scrollLine && d->cline < d->scrollLine + m_visRows &&
            d->ccol  >= d->scrollCol  && d->ccol  <= d->scrollCol + m_visCols) {
            float caretX = m_textX + colToX(d->lines[d->cline], d->scrollCol, d->ccol);
            float caretY = m_textTop + (d->cline - d->scrollLine) * LINE_H;
            addQuad(caretX, caretY, 1.0f, static_cast<float>(PixelFont::CELL_H), caretCol);
        }
    }

    upload();
}

void CodeEditor::upload() {
    if (!m_vertices.empty()) {
        m_vertexBuffer.uploadData(m_allocator, m_vertices.data(), m_vertices.size() * sizeof(UIVertex));
        m_indexBuffer.uploadData(m_allocator, m_indices.data(), m_indices.size() * sizeof(uint32_t));
        m_indexCount = static_cast<uint32_t>(m_indices.size());
    } else {
        m_indexCount = 0;
    }
}

void CodeEditor::draw(VkCommandBuffer cmd) {
    if (!isVisible() || m_indexCount == 0) return;
    VkBuffer buffers[] = { m_vertexBuffer.getBuffer() };
    VkDeviceSize offsets[] = { 0 };
    vkCmdBindVertexBuffers(cmd, 0, 1, buffers, offsets);
    vkCmdBindIndexBuffer(cmd, m_indexBuffer.getBuffer(), 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, m_indexCount, 1, 0, 0, 0);
}

void CodeEditor::drawImage(VkCommandBuffer cmd, const glm::vec2& screenSize) {
    Doc* d = active();
    if (!activeIsImage() || m_imgIndexCount == 0 || !d) return;
    VkBuffer vb[] = { m_imgVB.getBuffer() }; VkDeviceSize off[] = { 0 };
    vkCmdBindVertexBuffers(cmd, 0, 1, vb, off);
    vkCmdBindIndexBuffer(cmd, m_imgIB.getBuffer(), 0, VK_INDEX_TYPE_UINT32);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_imgPipeline->getPipelineLayout(), 0, 1, &d->descSet, 0, nullptr);
    ImagePush pc{}; pc.screenSize = screenSize; pc.mode = 0; pc.ballRadius = 1.0f;
    vkCmdPushConstants(cmd, m_imgPipeline->getPipelineLayout(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
    vkCmdDrawIndexed(cmd, 6, 1, 0, 0, 0);
}

void CodeEditor::drawSphere(VkCommandBuffer cmd) {
    Doc* d = active();
    if (!activeIsMaterial() || m_sphereIndexCount == 0 || m_sphereVp.z < 1.0f || !d) return;
    VkViewport vp{ m_sphereVp.x, m_sphereVp.y, m_sphereVp.z, m_sphereVp.w, 0.0f, 1.0f };
    VkRect2D   sc{ { (int32_t)m_sphereVp.x, (int32_t)m_sphereVp.y }, { (uint32_t)m_sphereVp.z, (uint32_t)m_sphereVp.w } };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    glm::mat4 view = glm::lookAt(glm::vec3(0, 0, 3), glm::vec3(0), glm::vec3(0, 1, 0));
    glm::mat4 proj = glm::perspective(glm::radians(40.0f), 1.0f, 0.1f, 10.0f);
    proj[1][1] *= -1.0f;
    PreviewPush pc{}; pc.viewProj = proj * view; pc.baseColor = d->baseColor; pc.params = { d->metallic, d->roughness, 0.0f, 0.0f };
    VkPipelineLayout pl = m_matPipeline->getPipelineLayout();
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &d->descSet, 0, nullptr);
    vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
    VkBuffer vb[] = { m_sphereVB.getBuffer() }; VkDeviceSize off[] = { 0 };
    vkCmdBindVertexBuffers(cmd, 0, 1, vb, off);
    vkCmdBindIndexBuffer(cmd, m_sphereIB.getBuffer(), 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, m_sphereIndexCount, 1, 0, 0, 0);
}

// ─── Selection / clipboard / undo ────────────────────────────────────────────

bool CodeEditor::hasSel(const Doc& d) const {
    return d.sel && (d.aline != d.cline || d.acol != d.ccol);
}

void CodeEditor::orderedSel(const Doc& d, int& sl, int& sc, int& el, int& ec) const {
    if (d.aline < d.cline || (d.aline == d.cline && d.acol <= d.ccol)) {
        sl = d.aline; sc = d.acol; el = d.cline; ec = d.ccol;
    } else {
        sl = d.cline; sc = d.ccol; el = d.aline; ec = d.acol;
    }
}

std::string CodeEditor::selText(const Doc& d) const {
    if (!hasSel(d)) return "";
    int sl, sc, el, ec; orderedSel(d, sl, sc, el, ec);
    if (sl == el) return d.lines[sl].substr(sc, ec - sc);
    std::string out = d.lines[sl].substr(sc) + "\n";
    for (int i = sl + 1; i < el; ++i) out += d.lines[i] + "\n";
    out += d.lines[el].substr(0, ec);
    return out;
}

void CodeEditor::deleteSel(Doc& d) {
    if (!hasSel(d)) return;
    int sl, sc, el, ec; orderedSel(d, sl, sc, el, ec);
    if (sl == el) {
        d.lines[sl].erase(sc, ec - sc);
    } else {
        d.lines[sl] = d.lines[sl].substr(0, sc) + d.lines[el].substr(ec);
        d.lines.erase(d.lines.begin() + sl + 1, d.lines.begin() + el + 1);
    }
    d.cline = sl; d.ccol = sc; d.sel = false; d.modified = true;
}

void CodeEditor::insertText(Doc& d, const std::string& text) {
    for (char ch : text) {
        if (ch == '\n') {
            std::string rest = d.lines[d.cline].substr(d.ccol);
            d.lines[d.cline].erase(d.ccol);
            d.lines.insert(d.lines.begin() + d.cline + 1, rest);
            d.cline++; d.ccol = 0;
        } else if (ch == '\r') {
            continue;
        } else if (ch == '\t') {
            d.lines[d.cline].insert(d.ccol, "    "); d.ccol += 4;
        } else if (ch >= 32 && ch < 127) {
            d.lines[d.cline].insert(d.ccol, 1, ch); d.ccol++;
        }
    }
    d.modified = true;
}

void CodeEditor::pushUndo(Doc& d, bool typing) {
    if (typing && d.coalesce) return;          // already grouping a typing burst
    d.undo.push_back({d.lines, d.cline, d.ccol});
    if (d.undo.size() > 300) d.undo.erase(d.undo.begin());
    d.redo.clear();
    d.coalesce = typing;
}

void CodeEditor::doUndo() {
    Doc* d = active();
    if (!d || d->undo.empty()) return;
    d->redo.push_back({d->lines, d->cline, d->ccol});
    UndoState st = d->undo.back(); d->undo.pop_back();
    d->lines = st.lines; d->cline = st.cline; d->ccol = st.ccol;
    d->sel = false; d->coalesce = false; d->modified = true;
    clampCursor(*d); ensureCursorVisible(); m_dirty = true;
}

void CodeEditor::doRedo() {
    Doc* d = active();
    if (!d || d->redo.empty()) return;
    d->undo.push_back({d->lines, d->cline, d->ccol});
    UndoState st = d->redo.back(); d->redo.pop_back();
    d->lines = st.lines; d->cline = st.cline; d->ccol = st.ccol;
    d->sel = false; d->coalesce = false; d->modified = true;
    clampCursor(*d); ensureCursorVisible(); m_dirty = true;
}

void CodeEditor::copySel(bool cut) {
    Doc* d = active();
    if (!d) return;
    bool sel = hasSel(*d);
    std::string text = sel ? selText(*d) : (d->lines[d->cline] + "\n");  // no selection → whole line
    glfwSetClipboardString(m_window, text.c_str());
    if (!cut) return;

    pushUndo(*d, false);
    if (sel) {
        deleteSel(*d);
    } else {
        if (d->lines.size() > 1) {
            d->lines.erase(d->lines.begin() + d->cline);
            if (d->cline >= (int)d->lines.size()) d->cline = (int)d->lines.size() - 1;
        } else {
            d->lines[0].clear();
        }
        d->ccol = 0;
    }
    d->sel = false; d->coalesce = false; d->modified = true;
    clampCursor(*d); ensureCursorVisible(); m_dirty = true;
}

void CodeEditor::paste() {
    Doc* d = active();
    if (!d) return;
    const char* cb = glfwGetClipboardString(m_window);
    if (!cb) return;
    pushUndo(*d, false);
    if (hasSel(*d)) deleteSel(*d);
    insertText(*d, std::string(cb));
    d->sel = false; d->coalesce = false;
    clampCursor(*d); ensureCursorVisible(); m_dirty = true;
}

void CodeEditor::selectAll() {
    Doc* d = active();
    if (!d) return;
    d->aline = 0; d->acol = 0; d->sel = true;
    d->cline = (int)d->lines.size() - 1;
    d->ccol  = (int)d->lines.back().size();
    ensureCursorVisible(); m_dirty = true;
}

// Lightweight generic highlighter: line comments (# // ), strings, numbers,
// keywords. Fills `out` with one color per character of `line`.
void CodeEditor::colorizeLine(const std::string& line, std::vector<glm::vec4>& out,
                              int& depth, bool& inBlock) const {
    static const glm::vec4 cDef  = {0.84f, 0.87f, 0.92f, 1.0f};
    static const glm::vec4 cCmt  = {0.45f, 0.58f, 0.46f, 1.0f};
    static const glm::vec4 cStr  = {0.88f, 0.66f, 0.42f, 1.0f};
    static const glm::vec4 cNum  = {0.46f, 0.72f, 0.96f, 1.0f};
    static const glm::vec4 cKey  = {0.80f, 0.55f, 0.95f, 1.0f};
    static const glm::vec4 cPun  = {0.58f, 0.61f, 0.68f, 1.0f};
    static const glm::vec4 cErr  = {0.95f, 0.32f, 0.32f, 1.0f};   // unmatched closing bracket
    // Rainbow palette cycled by nesting depth (() [] {} share the scheme).
    static const glm::vec4 cDepth[6] = {
        {0.96f, 0.79f, 0.33f, 1.0f},   // gold
        {0.85f, 0.47f, 0.92f, 1.0f},   // magenta
        {0.38f, 0.82f, 0.86f, 1.0f},   // cyan
        {0.58f, 0.86f, 0.46f, 1.0f},   // green
        {0.97f, 0.56f, 0.36f, 1.0f},   // orange
        {0.47f, 0.64f, 0.97f, 1.0f},   // periwinkle
    };
    auto bracket = [&](int d) -> const glm::vec4& { return cDepth[((d % 6) + 6) % 6]; };

    static const std::set<std::string> kw = {
        "int","float","double","char","bool","void","unsigned","short","long","auto","const","static",
        "struct","class","enum","union","namespace","template","typename","public","private","protected",
        "return","if","else","for","while","do","switch","case","break","continue","default","goto",
        "new","delete","this","true","false","nullptr","sizeof","using","typedef","virtual","override",
        "include","define","ifdef","ifndef","endif","pragma","import","from","function","let","var","const",
        "vec2","vec3","vec4","mat3","mat4","uniform","layout","in","out","gl_Position","float32","uint32"
    };

    int n = (int)line.size();
    out.assign(n, cDef);
    int i = 0;
    while (i < n) {
        char c = line[i];

        if (inBlock) {                                                     // inside /* ... */
            out[i] = cCmt;
            if (c == '*' && i + 1 < n && line[i + 1] == '/') { out[i + 1] = cCmt; i += 2; inBlock = false; continue; }
            i++; continue;
        }
        if (c == '/' && i + 1 < n && line[i + 1] == '*') {                 // block comment start
            out[i] = cCmt; out[i + 1] = cCmt; i += 2; inBlock = true; continue;
        }
        if (c == '#' || (c == '/' && i + 1 < n && line[i + 1] == '/')) {   // line comment
            for (; i < n; ++i) out[i] = cCmt;
            break;
        }
        if (c == '"' || c == '\'') {                                       // string / char literal
            char q = c; out[i++] = cStr;
            while (i < n) {
                out[i] = cStr;
                char ch = line[i++];
                if (ch == '\\' && i < n) { out[i] = cStr; i++; continue; }  // skip escaped char
                if (ch == q) break;
            }
            continue;
        }
        if (std::isdigit((unsigned char)c) ||
            (c == '.' && i + 1 < n && std::isdigit((unsigned char)line[i + 1]))) {  // number
            while (i < n && (std::isalnum((unsigned char)line[i]) || line[i] == '.')) out[i++] = cNum;
            continue;
        }
        if (std::isalpha((unsigned char)c) || c == '_') {                  // identifier / keyword
            int s = i;
            while (i < n && (std::isalnum((unsigned char)line[i]) || line[i] == '_')) i++;
            glm::vec4 col = kw.count(line.substr(s, i - s)) ? cKey : cDef;
            for (int j = s; j < i; ++j) out[j] = col;
            continue;
        }
        if (c == '(' || c == '[' || c == '{') {                            // open: color, then descend
            out[i] = bracket(depth); depth++; i++; continue;
        }
        if (c == ')' || c == ']' || c == '}') {                            // close: ascend, match open's color
            if (depth > 0) { depth--; out[i] = bracket(depth); }
            else           { out[i] = cErr; }                              // unmatched closer
            i++; continue;
        }
        if (c != ' ') out[i] = cPun;                                       // punctuation
        i++;
    }
}

// ─── Geometry helpers ────────────────────────────────────────────────────────

void CodeEditor::addQuad(float x, float y, float w, float h, const glm::vec4& color) {
    if (m_vertices.size() + 4 > VERT_CAP) return;
    const glm::vec2 z2{0.0f, 0.0f};
    const glm::vec4 z4{0.0f, 0.0f, 0.0f, 0.0f};
    uint32_t base = static_cast<uint32_t>(m_vertices.size());
    m_vertices.push_back({{x,     y},     color, z2, z4, z4});
    m_vertices.push_back({{x + w, y},     color, z2, z4, z4});
    m_vertices.push_back({{x + w, y + h}, color, z2, z4, z4});
    m_vertices.push_back({{x,     y + h}, color, z2, z4, z4});
    m_indices.push_back(base);     m_indices.push_back(base + 1); m_indices.push_back(base + 2);
    m_indices.push_back(base + 2); m_indices.push_back(base + 3); m_indices.push_back(base);
}

void CodeEditor::addGlyph(float x, float y, char c, const glm::vec4& color) {
    const uint8_t* rows = PixelFont::glyphRows(c);
    if (!rows) return;
    for (int r = 0; r < PixelFont::CELL_H; ++r) {
        uint8_t bits = rows[r];
        for (int col = 0; col < PixelFont::CELL_W; ++col) {
            if (bits & (1 << (PixelFont::CELL_W - 1 - col))) addQuad(x + col, y + r, 1.0f, 1.0f, color);
        }
    }
}

float CodeEditor::addText(float x, float y, const std::string& t, const glm::vec4& color, float maxX) {
    float cx = x;
    for (char c : t) {
        if (cx + PixelFont::CELL_W > maxX) break;
        addGlyph(cx, y, c, color);
        cx += CHAR_W;
    }
    return cx;
}

} // namespace Nyx
