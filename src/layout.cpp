#include "layout.hpp"

#include <algorithm>
#include <cmath>

#include "examples.hpp"
#include "lessons.hpp"
#include "progress.hpp"
#include "quiz.hpp"

namespace sparring {

Layout compute_layout(const Model& m, float width, float height) {
    Layout l;
    l.width = width;
    l.height = height;

    l.title_bar = {0, 0, width, kTitleBarHeight};
    l.app_icon = {16, (kTitleBarHeight - 16) / 2, 16, 16};
    l.caption_close = {width - kCaptionButtonWidth, 0, kCaptionButtonWidth, kTitleBarHeight};
    l.caption_maximize = {width - 2 * kCaptionButtonWidth, 0, kCaptionButtonWidth, kTitleBarHeight};
    l.caption_minimize = {width - 3 * kCaptionButtonWidth, 0, kCaptionButtonWidth, kTitleBarHeight};
    l.sound_button = {l.caption_minimize.x - 8 - 36, (kTitleBarHeight - 32) / 2, 36, 32};
    l.title_text = {44, 0, std::max(0.0f, l.sound_button.x - 8 - 44), kTitleBarHeight};

    // a running quest takes the whole window: no rail, no status bar, nothing to wander off to
    const bool quest_page = m.page == Page::Look || m.page == Page::Break || m.page == Page::Lesson || m.page == Page::Quiz;
    l.focus = m.quest.active && (m.quest.finished ? m.page == Page::QuestDone : quest_page);
    const float left = l.focus ? kOuterPadding : kRailWidth;

    if (l.focus) {
        l.status_bar = {0, height, width, 0};
    } else {
        l.status_bar = {0, height - kStatusBarHeight, width, kStatusBarHeight};
        l.status_dot = {kOuterPadding, l.status_bar.y + (kStatusBarHeight - 8) / 2, 8, 8};
        l.status_text = {kOuterPadding + 16, l.status_bar.y, width * 0.5f, kStatusBarHeight};
        l.status_right = {width * 0.5f, l.status_bar.y, std::max(0.0f, width * 0.5f - kOuterPadding), kStatusBarHeight};
        l.rail = {0, kTitleBarHeight, kRailWidth, std::max(0.0f, l.status_bar.y - kTitleBarHeight)};
        l.nav_home = {8, kTitleBarHeight + 4, 56, 56};
        l.nav_practice = {8, kTitleBarHeight + 4 + 60, 56, 56};
        l.nav_learn = {8, kTitleBarHeight + 4 + 120, 56, 56};
        l.nav_badges = {8, kTitleBarHeight + 4 + 180, 56, 56};
    }

    l.toast = {(width - 420) / 2, kTitleBarHeight + 12, 420, 68};

    float top = kTitleBarHeight + 4;
    const float wide = std::max(0.0f, width - left - kOuterPadding);
    if (m.quest.active && !m.quest.finished && quest_page) {
        l.quest_bar = {left, top, wide, 44};
        l.quest_quit = {l.quest_bar.right() - 40, l.quest_bar.y + 6, 32, 32};
        l.quest_skip = {l.quest_quit.x - 8 - 88, l.quest_bar.y + 6, 88, 32};
        top += 52;
    } else if (m.page == Page::Look || m.page == Page::Break || m.page == Page::Lesson) {
        // back to where it came from, and for a program its two steps
        l.session_bar = {left, top, wide, 44};
        l.session_back = {l.session_bar.x + 6, l.session_bar.y + 6, 96, 32};
        if (m.page != Page::Lesson || m.fixing) {
            l.step_fix = {l.session_bar.right() - 6 - 120, l.session_bar.y + 6, 120, 32};
            l.step_break = {l.step_fix.x - 16 - 120, l.session_bar.y + 6, 120, 32};
            l.step_look = {l.step_break.x - 16 - 128, l.session_bar.y + 6, 128, 32};
        }
        top += 52;
    }
    const float bottom = l.focus ? height - kOuterPadding : l.status_bar.y - 16;
    l.content = {left, top, wide, std::max(0.0f, bottom - top)};
    const Rect& c = l.content;

    auto split = [&](float left_share) {
        const float lw = std::floor((c.w - kCardGap) * left_share);
        l.card_a = {c.x, c.y, lw, c.h};
        l.card_b = {c.x + lw + kCardGap, c.y, std::max(0.0f, c.w - lw - kCardGap), c.h};
    };
    auto centered = [&](float w, float h) { return Rect{c.x + (c.w - w) / 2, c.y, w, h}; };

    switch (m.page) {
    case Page::Home: {
        // one big thing to press, and three doors under it
        const float door_h = std::clamp(c.h * 0.32f, 120.0f, 190.0f);
        l.hero = {c.x, c.y, c.w, std::max(0.0f, c.h - door_h - 16)};
        l.hero_side = {l.hero.right() - 40 - 320, l.hero.y + (l.hero.h - 156) / 2, 320, 156};
        l.hero_button = {l.hero.x + 44, l.hero.y + l.hero.h / 2 + 18, 340, 64};
        const float dw = std::max(0.0f, (c.w - 2 * 16) / 3);
        for (int i = 0; i < 3; ++i) l.doors[static_cast<std::size_t>(i)] = {c.x + static_cast<float>(i) * (dw + 16), l.hero.bottom() + 16, dw, door_h};
        break;
    }

    case Page::Practice: {
        l.card_a = c;
        const float inner = std::max(0.0f, c.w - 48);
        const int tabs = static_cast<int>(std::min(levels().size(), l.level_tabs.size()));
        const float tab_w = std::max(0.0f, (inner - static_cast<float>(tabs - 1) * 8) / static_cast<float>(tabs));
        for (int i = 0; i < tabs; ++i) l.level_tabs[static_cast<std::size_t>(i)] = {c.x + 24 + static_cast<float>(i) * (tab_w + 8), c.y + 84, tab_w, 40};
        l.examples_label = {c.x + 24, l.level_tabs[0].bottom() + 14, inner, 22};
        constexpr float drop_h = 80;
        const float ty = l.examples_label.bottom() + 10;
        const float th = std::clamp(c.bottom() - 24 - drop_h - 16 - ty, 150.0f, 360.0f);
        l.tiles_area = {c.x + 24, ty, inner, th};
        const std::vector<std::size_t> items = examples_in_level(m.pick_level);
        const float gap = 16;
        const float n = static_cast<float>(std::clamp<std::size_t>(items.size(), 3, 4));
        const float tw = std::max(0.0f, (inner - (n - 1) * gap) / n);
        for (std::size_t i = 0; i < items.size(); ++i) {
            l.examples.push_back({c.x + 24 + static_cast<float>(i) * (tw + gap), ty, tw, th});
            l.example_items.push_back(items[i]);
        }
        const float dy = ty + th + 16;
        l.drop_area = {c.x + 24, dy, std::max(0.0f, c.w - 48), drop_h};
        l.open_button = {l.drop_area.right() - 20 - 180, l.drop_area.y + (l.drop_area.h - 40) / 2, 180, 40};
        break;
    }

    case Page::Look: {
        split(0.56f);
        const Rect& b = l.card_b;
        l.code_view = {l.card_a.x + 16, l.card_a.y + 84, std::max(0.0f, l.card_a.w - 32), std::max(0.0f, l.card_a.h - 100)};
        if (m.replay_open) {
            l.replay_stop = {b.x + 24, b.bottom() - 64, std::max(0.0f, b.w - 48), 40};
            l.replay_back = {b.x + 24, b.y + 86, 44, 44};
            l.replay_play = {l.replay_back.right() + 8, b.y + 86, 64, 44};
            l.replay_next = {l.replay_play.right() + 8, b.y + 86, 44, 44};
            l.replay_count = {l.replay_next.right() + 16, b.y + 86, std::max(0.0f, b.right() - 24 - (l.replay_next.right() + 16)), 44};
            l.replay_bar = {b.x + 24, b.y + 142, std::max(0.0f, b.w - 48), 14};
            l.replay_console = {b.x + 24, l.replay_stop.y - 16 - 150, std::max(0.0f, b.w - 48), 150};
            l.replay_vars = {b.x + 24, b.y + 172, std::max(0.0f, b.w - 48), std::max(0.0f, l.replay_console.y - 16 - (b.y + 172))};
            break;
        }
        const float bw = std::max(0.0f, (b.w - 48 - 12) / 2);
        l.watch_button = {b.x + 24, b.bottom() - 64, bw, 40};
        l.next_step = {l.watch_button.right() + 12, b.bottom() - 64, bw, 40};
        float list_end = l.next_step.y - 32;
        if (m.program_reads_input) {
            l.input_box = {b.x + 24, l.watch_button.y - 14 - 64, std::max(0.0f, b.w - 48), 64};
            list_end = l.input_box.y - 26 - 22;
        }
        l.look_list = {b.x + 12, b.y + 84, std::max(0.0f, b.w - 24), std::max(0.0f, list_end - (b.y + 84))};
        const int fit = std::max(1, static_cast<int>(l.look_list.h / kLookRowHeight));
        const int total = static_cast<int>(m.look.size());
        l.look_first = std::clamp(m.look_scroll, 0, std::max(0, total - fit));
        for (int i = l.look_first; i < total && i < l.look_first + fit; ++i)
            l.rows.push_back({l.look_list.x, l.look_list.y + static_cast<float>(i - l.look_first) * kLookRowHeight, l.look_list.w, kLookRowHeight});
        l.look_hidden = std::max(0, total - l.look_first - fit);
        break;
    }

    case Page::Break: {
        split(0.54f);
        const Rect& a = l.card_a;
        if (!m.quest.active) {
            l.next_button = {a.right() - 24 - 36, a.y + 24, 36, 36};
            l.prev_button = {l.next_button.x - 8 - 36, a.y + 24, 36, 36};
        }
        if (m.quest.active && m.quest.boss) {
            l.boss_timer = {a.right() - 24 - 104, a.y + 20, 104, 28};
            l.boss_hud = {a.x + 24, a.y + 18, l.boss_timer.x - 12 - (a.x + 24), 32};
        }
        l.before_box = {a.x + 24, a.y + 112, std::max(0.0f, a.w - 48), 36};
        l.after_box = {a.x + 24, l.before_box.bottom() + 30, l.before_box.w, 36};
        l.why_text = {a.x + 24, l.after_box.bottom() + 14, l.before_box.w, 60};
        l.question = {a.x + 24, l.why_text.bottom() + 8, l.before_box.w, 26};
        const float gw = std::max(0.0f, (a.w - 48 - 12) / 2);
        const float gh = std::clamp((a.bottom() - 24 - (l.question.bottom() + 10) - 12) / 2, 60.0f, 124.0f);
        for (int i = 0; i < 4; ++i)
            l.guesses[static_cast<std::size_t>(i)] = {a.x + 24 + static_cast<float>(i % 2) * (gw + 12),
                                                      l.question.bottom() + 10 + static_cast<float>(i / 2) * (gh + 12), gw, gh};

        const Rect& b = l.card_b;
        l.banner = {b.x + 24, b.y + 84, std::max(0.0f, b.w - 48), 64};
        l.result_text = {b.x + 24, l.banner.bottom() + 10, l.banner.w, 44};
        l.real_life = {b.x + 24, l.result_text.bottom() + 4, l.banner.w, 52};
        const float bw = std::max(0.0f, (l.banner.w - 12) / 2);
        l.watch_changed = {b.x + 24, b.bottom() - 64, bw, 40};
        l.next_challenge = {l.watch_changed.right() + 12, b.bottom() - 64, bw, 40};
        const float oy = l.real_life.bottom() + 10;
        const float oh = std::max(0.0f, l.next_challenge.y - 16 - oy);
        const float ow = std::max(0.0f, (l.banner.w - 12) / 2);
        l.out_before = {b.x + 24, oy, ow, oh};
        l.out_after = {b.x + 24 + ow + 12, oy, ow, oh};
        break;
    }

    case Page::Badges: {
        l.card_a = c;
        const float sw = std::max(0.0f, (c.w - 48 - 3 * 12) / 4);
        for (int i = 0; i < 4; ++i) l.stats[static_cast<std::size_t>(i)] = {c.x + 24 + static_cast<float>(i) * (sw + 12), c.y + 88, sw, 76};
        const float gy = c.y + 88 + 76 + 20;
        constexpr int cols = 4;
        const int count = static_cast<int>(all_badges().size());
        const int rows = (count + cols - 1) / cols;
        const float bw = std::max(0.0f, (c.w - 48 - (cols - 1) * 10) / cols);
        const float bh = std::clamp((c.bottom() - 20 - gy - (rows - 1) * 10) / rows, 58.0f, 110.0f);
        for (int i = 0; i < count; ++i)
            l.badge_tiles.push_back({c.x + 24 + static_cast<float>(i % cols) * (bw + 10), gy + static_cast<float>(i / cols) * (bh + 10), bw, bh});
        break;
    }

    case Page::Lessons: {
        l.card_a = c;
        l.lessons_area = {c.x + 12, c.y + 84, std::max(0.0f, c.w - 24), std::max(0.0f, c.h - 96)};
        constexpr int cols = 3;
        const float tw = std::max(0.0f, (l.lessons_area.w - 24 - (cols - 1) * 10) / cols);
        float y = l.lessons_area.y - static_cast<float>(m.lessons_scroll);
        const float y0 = y;
        for (const Course& course : courses()) {
            l.course_heads.push_back({Rect{l.lessons_area.x + 12, y, l.lessons_area.w - 24, 30}, course.number});
            y += 36;
            int col = 0;
            for (std::size_t i = 0; i < lessons().size(); ++i) {
                if (lessons()[i].course != course.number) continue;
                l.lesson_tiles.push_back({l.lessons_area.x + 12 + static_cast<float>(col) * (tw + 10), y, tw, 56});
                l.lesson_items.push_back(static_cast<int>(i));
                if (++col == cols) {
                    col = 0;
                    y += 66;
                }
            }
            if (col != 0) y += 66;
            y += 10;
        }
        l.lessons_height = y - y0;
        break;
    }

    case Page::Lesson: {
        split(0.44f);
        const Rect& a = l.card_a;
        const Rect& b = l.card_b;
        l.lesson_text = {a.x + 24, a.y + 84, std::max(0.0f, a.w - 48), std::max(0.0f, a.h - 100)};
        l.lesson_next = {b.x + 16, b.bottom() - 56, std::max(0.0f, b.w - 32), 40};
        const float avail = l.lesson_next.y - 12 - (b.y + 84);
        l.editor = {b.x + 16, b.y + 84, std::max(0.0f, b.w - 32), std::max(120.0f, std::floor(avail * (m.lesson_reads_input ? 0.4f : 0.52f)))};
        const float by = l.editor.bottom() + 12;
        l.run_button = {b.x + 16, by, 132, 40};
        l.hint_button = {l.run_button.right() + 8, by, 140, 40};
        l.reset_button = {l.hint_button.right() + 8, by, 104, 40};
        float banner_y = by + 52;
        if (m.lesson_reads_input) {
            l.input_box = {b.x + 16, by + 52 + 22, std::max(0.0f, b.w - 32), 48};
            banner_y = l.input_box.bottom() + 10;
        }
        l.lesson_banner = {b.x + 16, banner_y, std::max(0.0f, b.w - 32), 50};
        l.lesson_out = {b.x + 16, l.lesson_banner.bottom() + 8, std::max(0.0f, b.w - 32), std::max(0.0f, l.lesson_next.y - 12 - (l.lesson_banner.bottom() + 8))};
        break;
    }

    case Page::Quiz: {
        l.quiz_card = centered(std::min(820.0f, c.w), c.h);
        const Rect& q = l.quiz_card;
        float y = q.y + 128;
        if (m.quiz >= 0 && m.quiz < static_cast<int>(quiz_cards().size()) && quiz_cards()[static_cast<std::size_t>(m.quiz)].code) {
            int lines = 1;
            for (const char* p = quiz_cards()[static_cast<std::size_t>(m.quiz)].code; *p; ++p) lines += *p == '\n';
            l.quiz_code = {q.x + 32, y, q.w - 64, static_cast<float>(lines) * 22 + 20};
            y = l.quiz_code.bottom() + 20;
        }
        const float aw = (q.w - 64 - 12) / 2;
        for (int i = 0; i < 4; ++i)
            l.quiz_answers[static_cast<std::size_t>(i)] = {q.x + 32 + static_cast<float>(i % 2) * (aw + 12), y + static_cast<float>(i / 2) * 76, aw, 64};
        l.quiz_why = {q.x + 32, y + 2 * 76 + 8, q.w - 64, 64};
        l.quiz_next = {q.x + 32, q.bottom() - 64, q.w - 64, 44};
        break;
    }

    case Page::Setup: {
        l.setup_card = centered(std::min(860.0f, c.w), c.h);
        const Rect& s = l.setup_card;
        for (int i = 0; i < 3; ++i) l.setup_rows[static_cast<std::size_t>(i)] = {s.x + 32, s.y + 104 + static_cast<float>(i) * 52, s.w - 64, 44};
        const float steps_y = l.setup_rows[2].bottom() + 24;
        l.setup_recheck = {s.x + 32, s.bottom() - 68, 200, 44};
        l.setup_installer = {l.setup_recheck.right() + 12, s.bottom() - 68, 280, 44};
        l.setup_continue = {s.right() - 32 - 170, s.bottom() - 68, 170, 44};
        l.setup_steps = {s.x + 32, steps_y, s.w - 64, std::max(0.0f, l.setup_recheck.y - 20 - steps_y)};
        break;
    }

    case Page::QuestDone: {
        l.done_card = centered(std::min(680.0f, c.w), c.h);
        const Rect& d = l.done_card;
        l.chest = {d.x + (d.w - 260) / 2, d.y + 236, 260, 140};
        const float bw = (d.w - 64 - 12) / 2;
        l.done_home = {d.x + 32, d.bottom() - 68, bw, 44};
        l.done_again = {l.done_home.right() + 12, d.bottom() - 68, bw, 44};
        break;
    }
    }
    return l;
}

}  // namespace sparring
