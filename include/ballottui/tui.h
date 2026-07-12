#ifndef BALLOTTUI_TUI_H
#define BALLOTTUI_TUI_H

#define TUI_MAX_ITEMS 16
#define TUI_MAX_FIELDS 8
#define TUI_FIELD_LEN 128

void tui_init(void);
void tui_shutdown(void);

/* status bar shown at the bottom of every screen */
void tui_set_status(const char *app, const char *actor, const char *state);
void tui_draw_status_bar(const char *hint);

/* arrow-key menu; returns selected index, or -1 if user pressed 'q'/ESC */
int tui_menu(const char *title, const char *items[], int count, const char *hint);

/* single-line text input; returns 0 on ok, -1 if cancelled (ESC) */
int tui_input(const char *title, const char *prompt, char *out, int out_len);

/* multi-field form; labels[count], values[count] pre-filled/edited in place.
 * returns 0 if submitted, -1 if cancelled (ESC) */
int tui_form(const char *title, const char *labels[], char values[][TUI_FIELD_LEN], int count);

/* yes/no confirmation; returns 1 for yes, 0 for no */
int tui_confirm(const char *title, const char *question);

/* simple message box, waits for a keypress */
void tui_message(const char *title, const char *lines[], int line_count);

/* scrollable read-only list view, waits for 'q'/ESC/ENTER */
void tui_list_view(const char *title, const char *lines[], int line_count);

/* fake multi-step progress animation, e.g. handshake or submit flows */
void tui_progress(const char *title, const char *steps[], int step_count);

#endif
