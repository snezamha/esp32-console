#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

class Canvas;
struct Theme;

struct MenuItem;
using MenuItems = std::vector<MenuItem>;

// One row of a menu page.
struct MenuItem {
  enum class Kind {
    Action,   // Runs on_select
    Toggle,   // Switch drawn from `toggle`; Power runs on_select
    Choice,   // Cycles through values with Power; value shown between arrows
    Adjust,   // Power opens an editor; Vol+ / Vol- call on_adjust(+1 / -1); Power closes it
    Hold,     // Power must be held for hold_ms; the app runs on_select when complete
    Submenu,  // Opens the page returned by build_submenu
    Info,     // Read-only row
    Back,     // Returns to the previous page
  };

  Kind kind = Kind::Action;
  std::string label;
  std::function<std::string()> value;           // Right-aligned text
  std::function<uint16_t(const Theme&)> color;  // Dot and value color; none = no dot
  std::function<void(Canvas&, int, int, uint16_t)> icon;  // 9×9 icon instead of a dot
  std::function<int()> level;     // 0–100 draws a meter instead of the value; <0 = none
  std::function<bool()> toggle;   // Switch state for Kind::Toggle
  std::function<int()> progress;  // 0–100 bar in the Adjust editor
  std::function<void()> on_select;
  std::function<void(int)> on_adjust;
  std::function<MenuItems()> build_submenu;
  std::string submenu_title;
  int hold_ms = 2000;
};

// Three-button menu with a page stack and smooth motion.
//   Vol+  next item (or +1 in the editor)
//   Vol-  previous item (or -1 in the editor)
//   Power select / confirm
class MenuView {
 public:
  bool IsOpen() const { return !pages_.empty(); }
  void Open(const std::string& title, std::function<MenuItems()> build);
  void Close();
  // Rebuilds the current page, keeping the selection (for live values and lists).
  void Refresh();

  void Next();
  void Previous();
  void Select();

  const std::string& Title() const;
  bool IsEditing() const { return editing_; }
  const MenuItem* SelectedItem() const;

  // Advances animations; returns true when the screen needs a redraw.
  bool Animate(uint32_t now_ms);
  void Draw(Canvas& canvas, int x, int y, int width, int height, const Theme& theme);

  // Called after every page change or Close().
  std::function<void()> on_change;

 private:
  struct Page {
    std::string title;
    std::function<MenuItems()> build;
    MenuItems items;
    int selected = 0;
  };

  void Push(const std::string& title, std::function<MenuItems()> build);
  void Pop();
  void SetSelected(int index);
  void ResetMotion(int slide_direction);
  Page* Current() { return pages_.empty() ? nullptr : &pages_.back(); }
  const Page* Current() const { return pages_.empty() ? nullptr : &pages_.back(); }
  void DrawRow(Canvas& c, const MenuItem& item, int index, int x, int row_y, int list_w,
               const Theme& theme);
  void DrawEditor(Canvas& c, const MenuItem& item, int x, int y, int w, int h, const Theme& theme);

  std::vector<Page> pages_;
  std::map<std::string, int> remembered_;  // Last selection per page title
  bool editing_ = false;

  float highlight_y_ = 0;
  float scroll_y_ = 0;
  float slide_x_ = 0;
  float editor_t_ = 0;  // 0 = closed, 1 = fully open
  int content_width_ = 128;
  int content_height_ = 112;
  uint32_t last_animate_ = 0;
  uint32_t selected_at_ = 0;  // For scrolling long labels
  bool marquee_active_ = false;
};
