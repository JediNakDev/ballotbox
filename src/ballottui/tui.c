#include "ballottui/tui.h"

#include <ncurses.h>
#include <string.h>

static char g_app[32] = "";
static char g_actor[64] = "(not logged in)";
static char g_state[32] = "";

void tui_init(void) {
  initscr();
  cbreak();
  noecho();
  keypad(stdscr, TRUE);
  curs_set(0);
}

void tui_shutdown(void) { endwin(); }

void tui_set_status(const char *app, const char *actor, const char *state) {
  if (app) strncpy(g_app, app, sizeof(g_app) - 1);
  if (actor) strncpy(g_actor, actor, sizeof(g_actor) - 1);
  if (state) strncpy(g_state, state, sizeof(g_state) - 1);
}

void tui_draw_status_bar(const char *hint) {
  int rows, cols;
  getmaxyx(stdscr, rows, cols);
  char buf[256];
  snprintf(buf, sizeof(buf), " %s | %s | %s | %s", g_app, g_actor,
           g_state[0] ? g_state : "-", hint ? hint : "");
  move(rows - 1, 0);
  clrtoeol();
  attron(A_REVERSE);
  mvhline(rows - 1, 0, ' ', cols);
  mvprintw(rows - 1, 0, "%.*s", cols, buf);
  attroff(A_REVERSE);
}

static WINDOW *frame_win(const char *title, int height, int width, int starty, int startx) {
  /* wipe remnants of any previously drawn (possibly larger) window */
  clear();
  refresh();
  WINDOW *win = newwin(height, width, starty, startx);
  box(win, 0, 0);
  if (title) {
    mvwprintw(win, 0, 2, " %s ", title);
  }
  return win;
}

int tui_menu(const char *title, const char *items[], int count, const char *hint) {
  int rows, cols;
  getmaxyx(stdscr, rows, cols);
  int height = count + 4;
  int width = cols - 10 > 40 ? cols - 10 : 40;
  int starty = (rows - height) / 2;
  int startx = (cols - width) / 2;
  WINDOW *win = frame_win(title, height, width, starty, startx);
  keypad(win, TRUE);

  int sel = 0;
  int ch;
  do {
    for (int i = 0; i < count; i++) {
      if (i == sel) wattron(win, A_REVERSE);
      mvwprintw(win, i + 2, 2, "%-*s", width - 4, items[i]);
      if (i == sel) wattroff(win, A_REVERSE);
    }
    tui_draw_status_bar(hint ? hint : "Up/Down move  Enter select  q back");
    wrefresh(win);
    refresh();
    ch = wgetch(win);
    if (ch == KEY_UP) sel = (sel - 1 + count) % count;
    else if (ch == KEY_DOWN) sel = (sel + 1) % count;
  } while (ch != '\n' && ch != 'q' && ch != 27);

  delwin(win);
  if (ch == 'q' || ch == 27) return -1;
  return sel;
}

int tui_input(const char *title, const char *prompt, char *out, int out_len) {
  int rows, cols;
  getmaxyx(stdscr, rows, cols);
  int width = cols - 10 > 50 ? cols - 10 : 50;
  int height = 6;
  int starty = (rows - height) / 2;
  int startx = (cols - width) / 2;
  WINDOW *win = frame_win(title, height, width, starty, startx);
  keypad(win, TRUE);
  mvwprintw(win, 2, 2, "%s", prompt);

  curs_set(1);
  char buf[TUI_FIELD_LEN];
  memset(buf, 0, sizeof(buf));
  int max = out_len - 1 < (int)sizeof(buf) - 1 ? out_len - 1 : (int)sizeof(buf) - 1;
  int cancelled = 0;
  for (;;) {
    mvwprintw(win, 3, 2, "%-*s", width - 4, buf);
    tui_draw_status_bar("Enter submit  Esc cancel");
    wmove(win, 3, 2 + (int)strlen(buf));
    wrefresh(win);
    int ch = wgetch(win);
    if (ch == 27) { cancelled = 1; break; }
    if (ch == '\n') break;
    if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
      int len = (int)strlen(buf);
      if (len > 0) buf[len - 1] = '\0';
      continue;
    }
    if (ch >= 32 && ch < 127 && (int)strlen(buf) < max) {
      int len = (int)strlen(buf);
      buf[len] = (char)ch;
      buf[len + 1] = '\0';
    }
  }
  curs_set(0);

  delwin(win);
  if (cancelled) return -1;
  strncpy(out, buf, out_len - 1);
  out[out_len - 1] = '\0';
  return 0;
}

int tui_form(const char *title, const char *labels[], char values[][TUI_FIELD_LEN], int count) {
  int rows, cols;
  getmaxyx(stdscr, rows, cols);
  int width = cols - 6 > 60 ? cols - 6 : 60;
  int height = count * 2 + 4;
  int starty = (rows - height) / 2;
  int startx = (cols - width) / 2;
  WINDOW *win = frame_win(title, height, width, starty, startx);
  keypad(win, TRUE);
  mvwprintw(win, height - 1, 2, " Enter submit | Esc cancel | Tab next field ");

  int field = 0;
  curs_set(1);
  int cancelled = 0;
  for (;;) {
    for (int i = 0; i < count; i++) {
      int y = i * 2 + 2;
      if (i == field) wattron(win, A_BOLD | A_UNDERLINE);
      mvwprintw(win, y, 2, "%s:", labels[i]);
      if (i == field) wattroff(win, A_BOLD | A_UNDERLINE);
      if (i == field) wattron(win, A_REVERSE);
      mvwprintw(win, y + 1, 2, "%-*s", width - 4, values[i]);
      if (i == field) wattroff(win, A_REVERSE);
    }
    tui_draw_status_bar("Tab/Down next field  Enter submit  Esc cancel");
    wmove(win, field * 2 + 3, 2 + (int)strlen(values[field]));
    wrefresh(win);
    refresh();
    int ch = wgetch(win);
    if (ch == 27) { cancelled = 1; break; }
    if (ch == '\n') break;
    if (ch == '\t' || ch == KEY_DOWN) { field = (field + 1) % count; continue; }
    if (ch == KEY_UP) { field = (field - 1 + count) % count; continue; }
    if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
      int len = (int)strlen(values[field]);
      if (len > 0) values[field][len - 1] = '\0';
      continue;
    }
    if (ch >= 32 && ch < 127) {
      int len = (int)strlen(values[field]);
      if (len < TUI_FIELD_LEN - 1) {
        values[field][len] = (char)ch;
        values[field][len + 1] = '\0';
      }
    }
  }
  curs_set(0);
  delwin(win);
  return cancelled ? -1 : 0;
}

int tui_confirm(const char *title, const char *question) {
  int rows, cols;
  getmaxyx(stdscr, rows, cols);
  int width = (int)strlen(question) + 10 > 50 ? (int)strlen(question) + 10 : 50;
  if (width > cols - 4) width = cols - 4;
  int height = 6;
  int starty = (rows - height) / 2;
  int startx = (cols - width) / 2;
  WINDOW *win = frame_win(title, height, width, starty, startx);
  keypad(win, TRUE);
  mvwprintw(win, 2, 2, "%s", question);

  int sel = 0; /* 0 = yes, 1 = no */
  int ch;
  do {
    const char *yes = "[ Yes ]";
    const char *no = "[ No ]";
    wattron(win, sel == 0 ? A_REVERSE : A_NORMAL);
    mvwprintw(win, 4, 2, "%s", yes);
    wattroff(win, sel == 0 ? A_REVERSE : A_NORMAL);
    wattron(win, sel == 1 ? A_REVERSE : A_NORMAL);
    mvwprintw(win, 4, 12, "%s", no);
    wattroff(win, sel == 1 ? A_REVERSE : A_NORMAL);
    tui_draw_status_bar("Left/Right choose  Enter confirm");
    wrefresh(win);
    refresh();
    ch = wgetch(win);
    if (ch == KEY_LEFT || ch == KEY_RIGHT) sel = 1 - sel;
  } while (ch != '\n');

  delwin(win);
  return sel == 0 ? 1 : 0;
}

void tui_message(const char *title, const char *lines[], int line_count) {
  int rows, cols;
  getmaxyx(stdscr, rows, cols);
  int width = cols - 10 > 50 ? cols - 10 : 50;
  int height = line_count + 4;
  int starty = (rows - height) / 2;
  int startx = (cols - width) / 2;
  WINDOW *win = frame_win(title, height, width, starty, startx);
  for (int i = 0; i < line_count; i++) {
    mvwprintw(win, i + 2, 2, "%.*s", width - 4, lines[i]);
  }
  tui_draw_status_bar("Press any key to continue");
  wrefresh(win);
  refresh();
  wgetch(win);
  delwin(win);
}

void tui_list_view(const char *title, const char *lines[], int line_count) {
  int rows, cols;
  getmaxyx(stdscr, rows, cols);
  int height = rows - 6 > 6 ? rows - 6 : 6;
  int width = cols - 10 > 50 ? cols - 10 : 50;
  int starty = (rows - height) / 2;
  int startx = (cols - width) / 2;
  WINDOW *win = frame_win(title, height, width, starty, startx);
  keypad(win, TRUE);

  int top = 0;
  int visible = height - 3;
  int ch;
  do {
    for (int row = 0; row < visible; row++) {
      mvwprintw(win, row + 2, 2, "%-*s", width - 4, "");
      int idx = top + row;
      if (idx < line_count) mvwprintw(win, row + 2, 2, "%.*s", width - 4, lines[idx]);
    }
    tui_draw_status_bar("Up/Down scroll  q/Enter back");
    wrefresh(win);
    refresh();
    ch = wgetch(win);
    if (ch == KEY_DOWN && top + visible < line_count) top++;
    else if (ch == KEY_UP && top > 0) top--;
  } while (ch != 'q' && ch != 27 && ch != '\n');

  delwin(win);
}

void tui_progress(const char *title, const char *steps[], int step_count) {
  int rows, cols;
  getmaxyx(stdscr, rows, cols);
  int width = cols - 10 > 50 ? cols - 10 : 50;
  int height = step_count + 4;
  int starty = (rows - height) / 2;
  int startx = (cols - width) / 2;
  WINDOW *win = frame_win(title, height, width, starty, startx);
  for (int i = 0; i < step_count; i++) {
    mvwprintw(win, i + 2, 2, "%s ...", steps[i]);
    wrefresh(win);
    napms(220);
    mvwprintw(win, i + 2, 2, "%s ... OK", steps[i]);
    wrefresh(win);
  }
  napms(300);
  delwin(win);
}
