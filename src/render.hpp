#pragma once
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <map>
#include <string>
#include <tuple>

#include "layout.hpp"
#include "palette.hpp"

namespace sparring {

using Microsoft::WRL::ComPtr;

struct Renderer {
    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> dwrite;
    ComPtr<ID2D1HwndRenderTarget> target;
    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<ID2D1StrokeStyle> dash;
    UINT dpi = 96;

    bool init();
    // The window's target, made again after the device is lost.
    bool ensure_target(HWND hwnd, UINT window_dpi);
    void resize(UINT width_px, UINT height_px);
    void set_dpi(UINT window_dpi);
    // Draws the whole window onto `rt` (the window's own target, or a bitmap for --screenshot); returns EndDraw's result.
    HRESULT paint(ID2D1RenderTarget* rt, const Model& model, const Layout& layout, const Palette& pal, bool backdrop);

    float measure(const std::wstring& s, int kind, float size, int weight);
    // How tall `s` is when wrapped to `width`.
    float text_height(const std::wstring& s, int kind, float size, int weight, float width);

    // kind: 0 body, 1 display, 2 icons, 3 monospace
    IDWriteTextFormat* format(int kind, float size, int weight, DWRITE_TEXT_ALIGNMENT align, DWRITE_PARAGRAPH_ALIGNMENT vertical, bool wrap);

private:
    using Key = std::tuple<int, int, int, int, int, bool>;
    std::map<Key, ComPtr<IDWriteTextFormat>> formats_;
    std::wstring body_, display_, icons_, mono_;
};

std::wstring widen(const std::string& utf8);
std::string narrow(const std::wstring& wide);

}  // namespace sparring
