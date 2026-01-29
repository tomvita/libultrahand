/********************************************************************************
 * File: tesla_lite.hpp
 * Description: 
 *   Lightweight version of tesla.hpp for simple overlays like the keyboard.
 *   Removes libultra dependencies, sound, haptics, and localization.
 ********************************************************************************/

#pragma once

#include <switch.h>
#include <arm_neon.h>
#include <strings.h>
#include <math.h>
#include <algorithm>
#include <cstring>
#include <string>
#include <functional>
#include <type_traits>
#include <mutex>
#include <memory>
#include <list>
#include <stack>
#include <vector>
#include <atomic>
#include <map>

#ifdef TESLA_INIT_IMPL
    #define STB_TRUETYPE_IMPLEMENTATION
#endif
#include "stb_truetype.h"

// Fallback declarations if libnx headers are not cooperating

// Map legacy KEY_* macros to modern libnx HidNpadButton_*
#define KEY_A           HidNpadButton_A
#define KEY_B           HidNpadButton_B
#define KEY_X           HidNpadButton_X
#define KEY_Y           HidNpadButton_Y
#define KEY_LSTICK      HidNpadButton_StickL
#define KEY_RSTICK      HidNpadButton_StickR
#define KEY_L           HidNpadButton_L
#define KEY_R           HidNpadButton_R
#define KEY_ZL          HidNpadButton_ZL
#define KEY_ZR          HidNpadButton_ZR
#define KEY_PLUS        HidNpadButton_Plus
#define KEY_MINUS       HidNpadButton_Minus
#define KEY_DLEFT       HidNpadButton_Left
#define KEY_DUP         HidNpadButton_Up
#define KEY_DRIGHT      HidNpadButton_Right
#define KEY_DDOWN       HidNpadButton_Down
#define KEY_SL          HidNpadButton_AnySL
#define KEY_SR          HidNpadButton_AnySR
#define KEY_UP          HidNpadButton_AnyUp
#define KEY_DOWN        HidNpadButton_AnyDown
#define KEY_LEFT        HidNpadButton_AnyLeft
#define KEY_RIGHT       HidNpadButton_AnyRight

#ifndef CONTROLLER_P1_AUTO
#define CONTROLLER_P1_AUTO 10
#endif

#define ELEMENT_BOUNDS(elem) elem->getX(), elem->getY(), elem->getWidth(), elem->getHeight()
#define ASSERT_EXIT(x) if (R_FAILED(x)) std::exit(1)
#define ASSERT_FATAL(x) if (Result res = x; R_FAILED(res)) fatalThrow(res)
#define PACKED __attribute__((packed))
#define ALWAYS_INLINE inline __attribute__((always_inline))

#define TSL_R_TRY(resultExpr)           \
    ({                                  \
        const auto result = resultExpr; \
        if (R_FAILED(result)) {         \
            return result;              \
        }                               \
    })

namespace tsl {

    namespace cfg {
        constexpr u32 ScreenWidth = 1920;
        constexpr u32 ScreenHeight = 1080;
        constexpr u32 LayerMaxWidth = 1280;
        constexpr u32 LayerMaxHeight = 720;
        
        inline u16 LayerWidth;
        inline u16 LayerHeight;
        inline u16 LayerPosX;
        inline u16 LayerPosY;
        inline u16 FramebufferWidth;
        inline u16 FramebufferHeight;
    }

    struct Color {
        union {
            struct {
                u16 r: 4, g: 4, b: 4, a: 4;
            } PACKED;
            u16 rgba;
        };
        constexpr inline Color(u16 raw): rgba(raw) {}
        constexpr inline Color(u8 r, u8 g, u8 b, u8 a): r(r), g(g), b(b), a(a) {}
    };

    namespace style {
        namespace color {
            constexpr Color ColorFrameBackground  = { 0x0, 0x0, 0x0, 0xD };
            constexpr Color ColorTransparent      = { 0x0, 0x0, 0x0, 0x0 };
            constexpr Color ColorHighlight        = { 0x0, 0xF, 0xD, 0xF };
            constexpr Color ColorFrame            = { 0x7, 0x7, 0x7, 0x7 };
            constexpr Color ColorText             = { 0xF, 0xF, 0xF, 0xF };
            constexpr Color ColorDescription      = { 0xA, 0xA, 0xA, 0xF };
            constexpr Color ColorClickAnimation   = { 0x0, 0x2, 0x2, 0xF };
        }
        constexpr u32 ListItemDefaultHeight = 70;
    }

    enum class FocusDirection { None, Up, Down, Left, Right };
    enum class InputMode { Controller, Touch, TouchScroll };

    inline bool overrideBackButton = false;
    inline bool disableJumpTo = false;
    inline bool disableHiding = false;

    namespace hlp {
        static inline ssize_t decode_utf8(u32 *out_cp, const u8 *in) {
            u8 code_unit = in[0];
            if (code_unit < 0x80) {
                *out_cp = code_unit;
                return 1;
            } else if (code_unit < 0xC2) {
                return -1;
            } else if (code_unit < 0xE0) {
                if ((in[1] & 0xC0) != 0x80) return -1;
                *out_cp = (u32(in[0] & 0x1F) << 6) | u32(in[1] & 0x3F);
                return 2;
            } else if (code_unit < 0xF0) {
                if ((in[1] & 0xC0) != 0x80 || (in[2] & 0xC0) != 0x80) return -1;
                *out_cp = (u32(in[0] & 0x0F) << 12) | (u32(in[1] & 0x3F) << 6) | u32(in[2] & 0x3F);
                return 3;
            } else if (code_unit < 0xF5) {
                if ((in[1] & 0xC0) != 0x80 || (in[2] & 0xC0) != 0x80 || (in[3] & 0xC0) != 0x80) return -1;
                *out_cp = (u32(in[0] & 0x07) << 18) | (u32(in[1] & 0x3F) << 12) | (u32(in[2] & 0x3F) << 6) | u32(in[3] & 0x3F);
                return 4;
            }
            return -1;
        }
    }

    namespace gfx {

        class FontManager {
        public:
            struct Glyph {
                u8* glyphBmp;
                u32 width, height;
                int xOffset, yOffset;
                float xAdvance;
                u32 currFontSize;
            };

            static bool isInitialized() { return s_initialized; }

            static void initialize() {
                if (s_initialized) return;
                
                // Load shared font from system
                // Load shared font from system
                static PlFontData fontData;
                if (R_SUCCEEDED(plGetSharedFontByType(&fontData, PlSharedFontType_Standard))) {
                    stbtt_InitFont(&s_stdFont, (const unsigned char*)fontData.address, 0);
                    s_initialized = true;
                }
            }

            static std::shared_ptr<Glyph> getOrCreateGlyph(u32 character, bool monospace, u32 fontSize) {
                u64 key = (u64(character) << 32) | fontSize;
                if (monospace) key |= (1ULL << 63);

                auto it = s_glyphCache.find(key);
                if (it != s_glyphCache.end()) return it->second;

                auto glyph = std::make_shared<Glyph>();
                glyph->currFontSize = fontSize;

                float scale = stbtt_ScaleForPixelHeight(&s_stdFont, fontSize);
                int glyphIndex = stbtt_FindGlyphIndex(&s_stdFont, character);
                
                int x0, y0, x1, y1;
                stbtt_GetGlyphBitmapBox(&s_stdFont, glyphIndex, scale, scale, &x0, &y0, &x1, &y1);
                
                glyph->width = x1 - x0;
                glyph->height = y1 - y0;
                glyph->xOffset = x0;
                glyph->yOffset = y0;
                
                int advance;
                stbtt_GetGlyphHMetrics(&s_stdFont, glyphIndex, &advance, nullptr);
                glyph->xAdvance = scale * advance;

                if (glyph->width > 0 && glyph->height > 0) {
                    glyph->glyphBmp = new u8[glyph->width * glyph->height];
                    stbtt_MakeGlyphBitmap(&s_stdFont, glyph->glyphBmp, glyph->width, glyph->height, glyph->width, scale, scale, glyphIndex);
                } else {
                    glyph->glyphBmp = nullptr;
                }

                s_glyphCache[key] = glyph;
                return glyph;
            }

        private:
            inline static stbtt_fontinfo s_stdFont;
            inline static bool s_initialized = false;
            inline static std::map<u64, std::shared_ptr<Glyph>> s_glyphCache;
        };

        class Renderer {
        public:
            static Renderer& get() {
                static Renderer instance;
                return instance;
            }

            void init() {
                FontManager::initialize();
            }

            static inline Color a(const Color& c) { return c; } // No fade for lite version

            void setPixel(u32 x, u32 y, Color color) {
                if (x >= cfg::FramebufferWidth || y >= cfg::FramebufferHeight) return;
                Color* fb = static_cast<Color*>(currFb);
                fb[y * cfg::FramebufferWidth + x] = color;
            }

            void setPixelBlend(u32 x, u32 y, Color color) {
                if (x >= cfg::FramebufferWidth || y >= cfg::FramebufferHeight || color.a == 0) return;
                Color* fb = static_cast<Color*>(currFb);
                u32 off = y * cfg::FramebufferWidth + x;
                Color src = fb[off];
                
                u8 inv_a = 15 - color.a;
                fb[off] = Color(
                    ((src.r * inv_a) + (color.r * color.a)) >> 4,
                    ((src.g * inv_a) + (color.g * color.a)) >> 4,
                    ((src.b * inv_a) + (color.b * color.a)) >> 4,
                    src.a // Keep destination alpha for overlays
                );
            }

            void drawRect(s32 x, s32 y, s32 w, s32 h, Color color) {
                s32 x_start = std::max(0, x);
                s32 y_start = std::max(0, y);
                s32 x_end = std::min<s32>(cfg::FramebufferWidth, x + w);
                s32 y_end = std::min<s32>(cfg::FramebufferHeight, y + h);
                
                for (s32 yi = y_start; yi < y_end; yi++) {
                    for (s32 xi = x_start; xi < x_end; xi++) {
                        setPixelBlend(xi, yi, color);
                    }
                }
            }

            // Simplified: Draw basic rect instead of rounded for lite performance/code size
            void drawRoundedRect(s32 x, s32 y, s32 w, s32 h, float radius, Color color) {
                drawRect(x, y, w, h, color);
            }
            
            void drawBorderedRoundedRect(s32 x, s32 y, s32 w, s32 h, float radius, float borderWidth, Color color) {
                // Draw outline only (naive implementation: full rect minus inner rect)
                // Actually, just draw 4 lines or a filled rect for now since overlay is simple
                drawRect(x, y, w, h, color);
            }

            void drawString(const std::string& text, bool monospace, s32 x, s32 y, u32 fontSize, Color color) {
                s32 currX = x;
                const char* p = text.c_str();
                while (*p) {
                    u32 cp;
                    ssize_t len = hlp::decode_utf8(&cp, reinterpret_cast<const u8*>(p));
                    if (len <= 0) break;
                    p += len;

                    if (cp == '\n') {
                        currX = x;
                        y += fontSize; // Simple line height
                        continue;
                    }

                    auto glyph = FontManager::getOrCreateGlyph(cp, monospace, fontSize);
                    if (glyph && glyph->glyphBmp) {
                        for (u32 gy = 0; gy < glyph->height; gy++) {
                            for (u32 gx = 0; gx < glyph->width; gx++) {
                                u8 alpha = glyph->glyphBmp[gy * glyph->width + gx];
                                if (alpha > 0) {
                                    Color c = color;
                                    c.a = (u8(color.a) * (alpha >> 4)) >> 4;
                                    setPixelBlend(currX + gx + glyph->xOffset, y + gy + glyph->yOffset + fontSize, c);
                                }
                            }
                        }
                    }
                    if (glyph) currX += glyph->xAdvance;
                }
            }

            std::pair<u32, u32> getTextDimensions(const std::string& text, bool monospace, u32 fontSize) {
                u32 width = 0;
                u32 height = fontSize; // Approximate height
                u32 maxWidth = 0;
                
                const char* p = text.c_str();
                while (*p) {
                    u32 cp;
                    ssize_t len = hlp::decode_utf8(&cp, reinterpret_cast<const u8*>(p));
                    if (len <= 0) break;
                    p += len;

                    if (cp == '\n') {
                        if (width > maxWidth) maxWidth = width;
                        width = 0;
                        height += fontSize;
                        continue;
                    }

                    auto glyph = FontManager::getOrCreateGlyph(cp, monospace, fontSize);
                    if (glyph) width += glyph->xAdvance;
                }
                if (width > maxWidth) maxWidth = width;
                return {maxWidth, height};
            }

            void* currFb = nullptr;
        };
    }
    
    // Global helper for alpha/color handling compatible with tesla.hpp usage
    inline Color a(const Color& c) { return c; }
    inline Color aWithOpacity(const Color& c) { return c; }

    namespace elm {

        class Element {
        public:
            Element() : m_x(0), m_y(0), m_width(0), m_height(0), m_focused(false), m_isItem(false), m_parent(nullptr) {}
            virtual ~Element() {}

            virtual void draw(gfx::Renderer* renderer) = 0;
            
            virtual void frame(gfx::Renderer* renderer) {
                if (m_focused && m_isItem) {
                    // Simple focus highlight
                     renderer->drawRect(getX() - 2, getY() - 2, getWidth() + 4, getHeight() + 4, style::color::ColorHighlight);
                }
                this->draw(renderer);
            }

            virtual void layout(u16 parentX, u16 parentY, u16 parentWidth, u16 parentHeight) {
                this->m_x = parentX;
                this->m_y = parentY;
                this->m_width = parentWidth;
                this->m_height = parentHeight;
            }

            virtual Element* requestFocus(FocusDirection direction) { return this; }
            virtual Element* requestFocus(Element* old, FocusDirection direction) { return requestFocus(direction); }

            virtual u16 getX() const { return m_x; }
            virtual u16 getY() const { return m_y; }
            virtual u16 getWidth() const { return m_width; }
            virtual s32 getHeight() { return m_height; } // Removed const to match keyboard.cpp

            void setBoundaries(u16 x, u16 y, u16 w, u16 h) {
                m_x = x; m_y = y; m_width = w; m_height = h;
            }

            virtual void invalidate() {
                if (m_parent) m_parent->invalidate(); 
            }

            virtual void setFocused(bool focused) { m_focused = focused; }
            bool isFocused() const { return m_focused; }

            virtual bool onClick(u64 keys) { return false; }
            
            void setParent(Element* parent) { m_parent = parent; }
            Element* getParent() const { return m_parent; }

        protected:
            u16 m_x, m_y, m_width, m_height;
            bool m_focused;
            bool m_isItem;
            Element* m_parent;
        };

        class List : public Element {
        public:
            List() : m_scrollOffset(0) {}
            virtual ~List() {
                for (auto item : m_items) delete item;
            }

            void addItem(Element* item) {
                m_items.push_back(item);
            }

            virtual void draw(gfx::Renderer* renderer) override {
                for (auto item : m_items) {
                    item->draw(renderer);
                }
            }

            // Simplified layout for List
            virtual void layout(u16 x, u16 y, u16 w, u16 h) override {
                Element::layout(x, y, w, h);
                u16 currY = y - m_scrollOffset;
                for (auto item : m_items) {
                    item->layout(x, currY, w, item->getHeight());
                    currY += item->getHeight();
                }
            }

        protected:
            std::vector<Element*> m_items;
            s32 m_scrollOffset;
        };

        class OverlayFrame : public Element {
        public:
            OverlayFrame(const std::string& title, const std::string& subtitle)
                : m_title(title), m_subtitle(subtitle), m_contentElement(nullptr) {
                m_isItem = false;
            }

            virtual ~OverlayFrame() {
                if (m_contentElement) delete m_contentElement;
            }

            void setContent(Element* content) {
                m_contentElement = content;
            }

            virtual void draw(gfx::Renderer* renderer) override {
                // Background
                renderer->drawRect(0, 0, cfg::FramebufferWidth, cfg::FramebufferHeight, style::color::ColorFrameBackground);
                
                // Header
                renderer->drawString(m_title, false, 20, 50, 32, style::color::ColorText);
                renderer->drawString(m_subtitle, false, 20, 85, 15, style::color::ColorDescription);
                
                // Content
                if (m_contentElement) m_contentElement->draw(renderer);
            }

            virtual void layout(u16 x, u16 y, u16 w, u16 h) override {
                Element::layout(x, y, w, h);
                if (m_contentElement) {
                    m_contentElement->layout(x + 20, y + 100, w - 40, h - 150);
                }
            }

        protected:
            std::string m_title;
            std::string m_subtitle;
            Element* m_contentElement;
        };

    }

    class Gui {
    public:
        Gui() {}
        virtual ~Gui() {
            if (m_root) delete m_root;
        }

        virtual elm::Element* createUI() = 0;
        virtual void update() {}

        virtual bool handleInput(u64 keysDown, u64 keysHeld, const HidTouchState& touch, HidAnalogStickState leftStick = {}, HidAnalogStickState rightStick = {}) { return false; }

        void updateGui() {
            if (!m_root) {
                m_root = createUI();
                if (m_root) {
                    m_root->layout(0, 0, cfg::FramebufferWidth, cfg::FramebufferHeight);
                }
            }
            update();
        }

        void draw(gfx::Renderer* renderer) {
            if (m_root) m_root->draw(renderer);
        }

        void requestFocus(elm::Element* element, FocusDirection direction) {
            if (m_focusedElement) m_focusedElement->setFocused(false);
            m_focusedElement = element;
            if (m_focusedElement) {
                m_focusedElement->setFocused(true);
            }
        }
        
        elm::Element* getFocusedElement() { return m_focusedElement; }

    protected:
        elm::Element* m_root = nullptr;
        elm::Element* m_focusedElement = nullptr;
    };

    class Overlay {
    public:
        static Overlay* get() { return s_instance; }

        Overlay() {
            s_instance = this;
            gfx::Renderer::get().init();
        }
        virtual ~Overlay() {
            while (!m_guiStack.empty()) {
                delete m_guiStack.top();
                m_guiStack.pop();
            }
            if (s_instance == this) s_instance = nullptr;
        }

        // Virtuals from keyboard.hpp
        virtual void initServices() {}
        virtual void exitServices() {}
        virtual void onShow() {}
        virtual void onHide() {}
        virtual std::unique_ptr<Gui> loadInitialGui() { return nullptr; }

        void pushGui(Gui* gui) {
            m_guiStack.push(gui);
        }
        
        void changeGui(Gui* gui) {
             if (!m_guiStack.empty()) {
                 delete m_guiStack.top();
                 m_guiStack.pop();
             }
             m_guiStack.push(gui);
        }

        Gui* getCurrentGui() {
            return m_guiStack.empty() ? nullptr : m_guiStack.top();
        }
        
        void close() {
            m_shouldClose = true;
        }
        
        bool shouldClose() const { return m_shouldClose; }

        void loop() {
            if (m_guiStack.empty()) {
                auto initial = loadInitialGui();
                if (initial) pushGui(initial.release());
                else return;
            }
            
            Gui* gui = m_guiStack.top();
            gui->updateGui();
            
            gfx::Renderer::get().currFb = framebufferBegin(&m_fb, nullptr);
            gfx::Renderer::get().drawRect(0, 0, cfg::FramebufferWidth, cfg::FramebufferHeight, style::color::ColorFrameBackground);
            gui->draw(&gfx::Renderer::get());
            framebufferEnd(&m_fb);
        }

    private:
        std::stack<Gui*> m_guiStack;
        Framebuffer m_fb;
        bool m_shouldClose = false;
        static Overlay* s_instance;
    };
    
    inline Overlay* Overlay::s_instance = nullptr;

    inline void setNextOverlay(const std::string& ovlPath, const std::string& args = "") {}

    template<typename T, typename... Args>
    void swapTo(Args&&... args) {
        T* newGui = new T(std::forward<Args>(args)...);
        Overlay::get()->changeGui(newGui);
    }

    template<typename TOverlay>
    int loop(int argc, char** argv) {
        TOverlay* overlay = new TOverlay();
        overlay->initServices();
        
        PadState pad;
        padInitializeDefault(&pad);
        hidInitializeTouchScreen();
        
        // Simplified loop logic
        while (appletMainLoop()) {
            if (overlay->shouldClose()) break;
            
            padUpdate(&pad);
            u64 kDown = padGetButtonsDown(&pad);
            u64 kHeld = padGetButtons(&pad);
            
            HidTouchScreenState touchState = {0};
            HidTouchState touch = {0};
            if (hidGetTouchScreenStates(&touchState, 1)) {
                if (touchState.count > 0) {
                    touch = touchState.touches[0];
                }
            }
            
            if (auto gui = overlay->getCurrentGui()) {
                gui->handleInput(kDown, kHeld, touch);
            }
            
            overlay->loop();
        }
        
        overlay->exitServices();
        delete overlay;
        return 0;
    }

}
