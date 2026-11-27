#pragma once
// A PDF text reader written from scratch (no libraries): it gives back, for every page, the lines of text with each line's font size,
// whether it is bold-ish and whether it is in a monospace (code) font. Classic cross-reference tables, cross-reference streams, object
// streams, incremental updates and Flate are all understood. Pictures are ignored (a scanned book has no text to read), and a PDF
// that needs a password is refused with a plain message.
#include <functional>
#include <string>
#include <vector>

namespace sparring::pdf {

struct Run {
    std::string text;   // UTF-8
    float size = 0;     // font size in points
    bool bold = false;
    bool mono = false;
};

struct Line {
    std::vector<Run> runs;   // neighbours with the same look are joined
    std::string text;        // the whole line (a gap between words is a space; in code, the gap is as many spaces as fit)
    float x = 0;             // where the first letter starts (points from the left edge)
    float x_end = 0;         // where the last letter ends
    float y = 0;             // the baseline (points from the bottom edge)
    float size = 0;          // the font size most of the letters have
    bool bold = false;       // most of the letters are bold
    bool mono = false;       // most of the letters are in a monospace font
    float char_w = 0;        // monospace lines: how wide one letter is
};

struct Page {
    float width = 612, height = 792;
    std::vector<Line> lines;   // top to bottom
};

struct Document {
    bool ok = false;
    bool cancelled = false;
    std::string error;   // plain words, set when ok is false
    std::string title;   // from the file's Info, may be empty
    std::vector<Page> pages;
};

// Called after each page with (pages read, pages in all); return false to stop.
using ProgressFn = std::function<bool(int, int)>;

Document read_bytes(const std::string& bytes, const ProgressFn& progress = {});
Document read_file(const std::wstring& path, const ProgressFn& progress = {});

// The Flate (zlib/deflate) decoder, public so it can be tested. `zlib` says the data starts with the 2-byte zlib header.
// Returns true when the whole stream was decoded; `out` holds what was decoded either way. `limit` bounds the output.
bool inflate(const unsigned char* data, std::size_t size, std::string& out, bool zlib = true, std::size_t limit = 256u * 1024 * 1024);

}  // namespace sparring::pdf

namespace sparring {
int pdf_dump(const std::wstring& in, const std::wstring& out);   // development helper (cppsparring --pdf-text IN OUT)
int pdf_fuzz(const std::wstring& in, int rounds);                // development helper (cppsparring --pdf-fuzz IN ROUNDS)
}
