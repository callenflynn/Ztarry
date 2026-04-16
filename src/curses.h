#pragma once

#include <cstdarg>

#ifdef __cplusplus
extern "C" {
#endif

struct WINDOW;

extern WINDOW *stdscr;
extern int LINES;
extern int COLS;

#define TRUE 1
#define FALSE 0

#define COLOR_BLACK 0
#define COLOR_RED 1
#define COLOR_GREEN 2
#define COLOR_YELLOW 3
#define COLOR_BLUE 4
#define COLOR_MAGENTA 5
#define COLOR_CYAN 6
#define COLOR_WHITE 7

#define A_NORMAL 0x0000
#define A_BOLD 0x0001
#define A_REVERSE 0x0002

#define COLOR_PAIR(n) ((n) << 8)

#define KEY_UP 1001
#define KEY_DOWN 1002
#define KEY_LEFT 1003
#define KEY_RIGHT 1004
#define KEY_ENTER 1005

WINDOW *initscr(void);
int cbreak(void);
int noecho(void);
int curs_set(int visibility);
int keypad(WINDOW *window, int enabled);
int nodelay(WINDOW *window, int enabled);
int has_colors(void);
int start_color(void);
int use_default_colors(void);
int init_pair(short pair, short fg, short bg);
int attron(int attrs);
int attroff(int attrs);
int mvaddch(int y, int x, int ch);
int mvhline(int y, int x, int ch, int n);
int mvprintw(int y, int x, const char *fmt, ...);
int erase(void);
int wnoutrefresh(WINDOW *window);
int doupdate(void);
int curses_getch(void);
int endwin(void);

#ifdef __cplusplus
}
#endif
