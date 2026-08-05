#include <stddef.h>

#include "libballotui/ballotui.h"
#include "ballotctl/mock.h"
#include "ballotctl/screens.h"

int main(void) {
  mock_init();
  ballotui_init();
  ballotui_set_status("ballotctl", "(not logged in)", "");

  if (screen_login()) {
    const char *items[] = {"Create election (UC-1)", "Open election", "Close election",
                            "Publish results", "View results (UC-5)", "Election status", "Quit"};
    for (;;) {
      int sel = ballotui_menu("ballotctl - admin menu", items, 7, NULL);
      if (sel < 0 || sel == 6) break;
      switch (sel) {
        case 0: screen_create_election(); break;
        case 1: screen_open_election(); break;
        case 2: screen_close_election(); break;
        case 3: screen_publish_results(); break;
        case 4: screen_view_results(); break;
        case 5: screen_election_status(); break;
      }
    }
  }

  ballotui_shutdown();
  return 0;
}
