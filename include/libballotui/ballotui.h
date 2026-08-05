#ifndef LIBBALLOTUI_BALLOTUI_H
#define LIBBALLOTUI_BALLOTUI_H

#define BALLOTUI_MAX_ITEMS 16
#define BALLOTUI_MAX_FIELDS 8
#define BALLOTUI_FIELD_LEN 128

void ballotui_init(void);
void ballotui_shutdown(void);

/* status bar shown at the bottom of every screen */
void ballotui_set_status(const char *app, const char *actor, const char *state);
void ballotui_draw_status_bar(const char *hint);

/* arrow-key menu; returns selected index, or -1 if user pressed 'q'/ESC */
int ballotui_menu(const char *title, const char *items[], int count, const char *hint);

/* single-line text input; returns 0 on ok, -1 if cancelled (ESC) */
int ballotui_input(const char *title, const char *prompt, char *out, int out_len);

/* multi-field form; labels[count], values[count] pre-filled/edited in place.
 * returns 0 if submitted, -1 if cancelled (ESC) */
int ballotui_form(const char *title, const char *labels[], char values[][BALLOTUI_FIELD_LEN], int count);

/* yes/no confirmation; returns 1 for yes, 0 for no */
int ballotui_confirm(const char *title, const char *question);

/* simple message box, waits for a keypress */
void ballotui_message(const char *title, const char *lines[], int line_count);

/* scrollable read-only list view, waits for 'q'/ESC/ENTER */
void ballotui_list_view(const char *title, const char *lines[], int line_count);

/* fake multi-step progress animation, e.g. handshake or submit flows */
void ballotui_progress(const char *title, const char *steps[], int step_count);

#endif
