#pragma once

// CodeEditor.h — Central tabbed view. Every opened file is a closable tab in one
// tab bar: TEXT tabs are an editable code view (syntax-highlighted via tree-sitter);
// IMAGE / MATERIAL / BINARY tabs are read-only asset previews (flat image, a lit
// textured sphere, or an info card). Text/chrome render through the UIPipeline;
// images through ImagePipeline; the material sphere through MaterialPreviewPipeline.

#include "ui/UIVertex.h"
#include "ui/Highlighter.h"
#include "renderer/Buffer.h"
#include "renderer/Texture.h"
#include "renderer/ImagePipeline.h"
#include "renderer/MaterialPreviewPipeline.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <vector>
#include <string>
#include <cstdint>

namespace Nyx {

class VulkanContext;

class CodeEditor {
public:
    static constexpr float TABBAR_H = 22.0f;
    static constexpr uint32_t VERT_CAP = 131072;

    void init(VulkanContext& context, GLFWwindow* window,
              ImagePipeline* imgPipeline, MaterialPreviewPipeline* matPipeline);
    void cleanup(VmaAllocator allocator);

    void openFile(const std::string& path);       // text file → editable tab
    void save();                                   // save the active text tab
    void saveAll();                                // save all modified text tabs
    void openImage(const std::string& path);       // image → flat preview tab
    void openMaterial(const std::string& path);    // .mat → sphere preview tab
    void openBinary(const std::string& path);      // other → info-card tab
    void closePath(const std::string& path);        // close tab(s) for a deleted file/folder

    void update(float left, float top, float right, float bottom);
    void draw(VkCommandBuffer cmd);                                   // tab bar + text/chrome (UIPipeline)
    void drawImage(VkCommandBuffer cmd, const glm::vec2& screenSize); // active image tab (ImagePipeline)
    void drawSphere(VkCommandBuffer cmd);                            // active material tab (MaterialPreviewPipeline)

    bool hasDocs()   const { return !m_docs.empty(); }
    bool isVisible() const { return m_visible && !m_docs.empty(); }
    void setVisible(bool v) { m_visible = v; }
    void setFocused(bool f) { if (m_focused != f) { m_focused = f; m_dirty = true; } }
    bool isFocused() const { return m_focused && isVisible() && activeIsText(); }
    bool activeIsImage()    const;
    bool activeIsMaterial() const;

    // Input
    bool handleMouseButton(int button, int action, int mods);
    void handleChar(unsigned int codepoint);
    void handleKey(int key, int action, int mods);
    bool handleScroll(double yoffset);

private:
    enum class Kind { Text, Image, Material, Binary };
    struct UndoState { std::vector<std::string> lines; int cline, ccol; };
    struct Doc {
        Kind        kind = Kind::Text;
        std::string path;
        std::string name;
        // Text
        std::vector<std::string> lines;   // never empty (≥1 line) for text
        int  cline = 0, ccol = 0;
        int  aline = 0, acol = 0;
        bool sel = false;
        int  scrollLine = 0, scrollCol = 0;
        bool modified = false;
        std::vector<UndoState> undo, redo;
        bool coalesce = false;
        // Asset (image / material) GPU resources — freed explicitly in closeTab.
        Texture         texture;
        VkDescriptorSet descSet = VK_NULL_HANDLE;
        bool            hasTex  = false;
        uint32_t        imgW = 0, imgH = 0;
        uint64_t        fileSize = 0;
        glm::vec4       baseColor{1.0f};
        float           metallic = 0.0f, roughness = 0.5f;
        std::string     albedoName;
    };

    VulkanContext*           m_context   = nullptr;
    VkDevice                 m_device    = VK_NULL_HANDLE;
    VmaAllocator             m_allocator = VK_NULL_HANDLE;
    GLFWwindow*              m_window    = nullptr;
    ImagePipeline*           m_imgPipeline = nullptr;
    MaterialPreviewPipeline* m_matPipeline = nullptr;
    VkDescriptorPool         m_descPool  = VK_NULL_HANDLE;
    bool         m_visible   = true;
    bool         m_focused   = false;

    std::vector<Doc> m_docs;
    int              m_active = -1;

    // Layout (set each update).
    float m_x = 0, m_y = 0, m_w = 0, m_h = 0;
    float m_gutterW = 0, m_textX = 0, m_textTop = 0;
    int   m_visRows = 0, m_visCols = 0;

    // UIPipeline geometry (tab bar + text + asset chrome/cards).
    Buffer   m_vertexBuffer, m_indexBuffer;
    bool     m_buffersInitialized = false;
    uint32_t m_indexCount = 0;
    std::vector<UIVertex> m_vertices;
    std::vector<uint32_t> m_indices;

    // Flat-image quad (active image tab) + static sphere mesh (material tabs).
    Buffer   m_imgVB, m_imgIB;  uint32_t m_imgIndexCount = 0;
    Buffer   m_sphereVB, m_sphereIB; uint32_t m_sphereIndexCount = 0;
    std::vector<ImageVertex> m_imgVerts; std::vector<uint32_t> m_imgIdx;
    glm::vec4 m_sphereVp{0.0f};   // material sphere viewport (x,y,w,h)

    bool  m_dirty = true;
    float m_lastX = -1, m_lastY = -1, m_lastW = -1, m_lastH = -1;

    Highlighter m_hl;
    int         m_hlDoc  = -1;
    std::string m_hlText;

    Doc*  active() { return (m_active >= 0 && m_active < (int)m_docs.size()) ? &m_docs[m_active] : nullptr; }
    const Doc* active() const { return (m_active >= 0 && m_active < (int)m_docs.size()) ? &m_docs[m_active] : nullptr; }
    bool  activeIsText() const { const Doc* d = active(); return d && d->kind == Kind::Text; }
    std::string tabLabel(const Doc& d) const;
    float       tabW(const Doc& d) const;
    void  saveDoc(Doc& d);
    void  closeTab(int i);
    void  ensureCursorVisible();
    void  clampCursor(Doc& d);

    // Asset helpers.
    int   findOrAdd(const std::string& path, Kind kind);   // returns doc index, focuses it
    bool  loadDocTexture(Doc& d, const std::string& imgPath);
    void  buildSphere();
    void  freeDocAsset(Doc& d);
    static std::string humanSize(uint64_t bytes);
    void  addImageQuad(float x, float y, float w, float h);

    // Selection / clipboard / undo
    bool        hasSel(const Doc& d) const;
    void        orderedSel(const Doc& d, int& sl, int& sc, int& el, int& ec) const;
    std::string selText(const Doc& d) const;
    void        deleteSel(Doc& d);
    void        insertText(Doc& d, const std::string& text);
    void        pushUndo(Doc& d, bool typing);
    void        doUndo();
    void        doRedo();
    void        copySel(bool cut);
    void        paste();
    void        selectAll();
    void        colorizeLine(const std::string& line, std::vector<glm::vec4>& out,
                             int& depth, bool& inBlock) const;

    void  addQuad(float x, float y, float w, float h, const glm::vec4& color);
    void  addGlyph(float x, float y, char c, const glm::vec4& color);
    float addText(float x, float y, const std::string& t, const glm::vec4& color, float maxX);
    void  upload();
};

} // namespace Nyx
