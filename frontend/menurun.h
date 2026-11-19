#ifndef FRONTEND_MENURUN_H
#define FRONTEND_MENURUN_H

/* Koriki: the game keeps running behind the menu in a link session (menurun.c) */
extern volatile int menu_run_active;   /* set while the thread runs the game */
void menu_run_begin(void);
void menu_run_end(void);

#endif
