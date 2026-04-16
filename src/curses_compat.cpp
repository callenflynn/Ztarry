#define NOMINMAX
#include "curses.h"

#include <windows.h>
#include <conio.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace {

struct Cell {
    char ch = ' ';
    WORD attr = 7;
};

struct PairDef {
    short fg = COLOR_WHITE;
    short bg = -1;
};

struct State {
    bool initialized = false;
    bool colorsEnabled = false;
    bool keypadEnabled = false;
    bool nodelayEnabled = true;
    bool echoEnabled = false;
    bool cursorVisible = false;
    short activePair = 0;
    int activeStyle = A_NORMAL;
    WORD defaultAttr = 7;
    HANDLE outHandle = INVALID_HANDLE_VALUE;
    HANDLE inHandle = INVALID_HANDLE_VALUE;
    int cols = 120;
    int lines = 40;
    std::vector<Cell> cells;
    PairDef pairs[256];
} g_state;

WORD map_color(short color, bool background) {
    switch (color) {
        case COLOR_BLACK: return 0;
        case COLOR_RED: return background ? BACKGROUND_RED : FOREGROUND_RED;
        case COLOR_GREEN: return background ? BACKGROUND_GREEN : FOREGROUND_GREEN;
        case COLOR_YELLOW: return background ? (BACKGROUND_RED | BACKGROUND_GREEN) : (FOREGROUND_RED | FOREGROUND_GREEN);
        case COLOR_BLUE: return background ? BACKGROUND_BLUE : FOREGROUND_BLUE;
        case COLOR_MAGENTA: return background ? (BACKGROUND_RED | BACKGROUND_BLUE) : (FOREGROUND_RED | FOREGROUND_BLUE);
        case COLOR_CYAN: return background ? (BACKGROUND_GREEN | BACKGROUND_BLUE) : (FOREGROUND_GREEN | FOREGROUND_BLUE);
        case COLOR_WHITE: return background ? (BACKGROUND_RED | BACKGROUND_GREEN | BACKGROUND_BLUE) : (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
        default: return 0;
    }
}

WORD compute_attr(short pairIndex, int style) {
    WORD attr = g_state.defaultAttr;
    if (pairIndex > 0 && pairIndex < 256) {
        const PairDef &pair = g_state.pairs[pairIndex];
        attr = map_color(pair.fg, false);
        if (pair.bg >= 0) {
            attr |= map_color(pair.bg, true);
        }
    }

    if (style & A_BOLD) {
        attr |= FOREGROUND_INTENSITY;
    }
    if (style & A_REVERSE) {
        WORD fg = attr & (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY);
        WORD bg = attr & (BACKGROUND_RED | BACKGROUND_GREEN | BACKGROUND_BLUE);
        attr &= ~(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY | BACKGROUND_RED | BACKGROUND_GREEN | BACKGROUND_BLUE);
        if (fg & FOREGROUND_RED) attr |= BACKGROUND_RED;
        if (fg & FOREGROUND_GREEN) attr |= BACKGROUND_GREEN;
        if (fg & FOREGROUND_BLUE) attr |= BACKGROUND_BLUE;
        if (fg & FOREGROUND_INTENSITY) attr |= (BACKGROUND_RED | BACKGROUND_GREEN | BACKGROUND_BLUE);
        if (bg & BACKGROUND_RED) attr |= FOREGROUND_RED;
        if (bg & BACKGROUND_GREEN) attr |= FOREGROUND_GREEN;
        if (bg & BACKGROUND_BLUE) attr |= FOREGROUND_BLUE;
    }

    return attr;
}

void ensure_buffer_size() {
    const size_t required = static_cast<size_t>(std::max(1, g_state.lines)) * static_cast<size_t>(std::max(1, g_state.cols));
    if (g_state.cells.size() != required) {
        g_state.cells.assign(required, Cell{});
    }
}

void update_console_size() {
    if (g_state.outHandle == INVALID_HANDLE_VALUE) {
        return;
    }

    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (GetConsoleScreenBufferInfo(g_state.outHandle, &info)) {
        const int cols = std::max(20, static_cast<int>(info.srWindow.Right - info.srWindow.Left + 1));
        const int lines = std::max(10, static_cast<int>(info.srWindow.Bottom - info.srWindow.Top + 1));
        if (cols != g_state.cols || lines != g_state.lines) {
            g_state.cols = cols;
            g_state.lines = lines;
            ensure_buffer_size();
        }
    }
}

int render_cell(int y, int x, char ch, WORD attr, WORD &lastAttr) {
    if (g_state.outHandle == INVALID_HANDLE_VALUE) {
        return 0;
    }

    if (attr != lastAttr) {
        SetConsoleTextAttribute(g_state.outHandle, attr);
        lastAttr = attr;
    }

    COORD coord{};
    coord.X = static_cast<SHORT>(x);
    coord.Y = static_cast<SHORT>(y);
    DWORD written = 0;
    return WriteConsoleOutputCharacterA(g_state.outHandle, &ch, 1, coord, &written) ? 0 : -1;
}

} // namespace

WINDOW *stdscr = nullptr;
int LINES = 40;
int COLS = 120;

struct WINDOW {};

WINDOW *initscr(void) {
    if (!g_state.initialized) {
        g_state.outHandle = GetStdHandle(STD_OUTPUT_HANDLE);
        g_state.inHandle = GetStdHandle(STD_INPUT_HANDLE);
        if (g_state.outHandle != INVALID_HANDLE_VALUE) {
            CONSOLE_SCREEN_BUFFER_INFO info{};
            if (GetConsoleScreenBufferInfo(g_state.outHandle, &info)) {
                g_state.defaultAttr = info.wAttributes;
            }
        }
        g_state.initialized = true;
        update_console_size();
        ensure_buffer_size();
        stdscr = reinterpret_cast<WINDOW *>(&g_state);
    }
    return stdscr;
}

int cbreak(void) { return 0; }
int noecho(void) { return 0; }
int curs_set(int visibility) { g_state.cursorVisible = visibility != 0; return 0; }
int keypad(WINDOW *, int enabled) { g_state.keypadEnabled = enabled != 0; return 0; }
int nodelay(WINDOW *, int enabled) { g_state.nodelayEnabled = enabled != 0; return 0; }
int has_colors(void) { return 1; }
int start_color(void) { g_state.colorsEnabled = true; return 0; }
int use_default_colors(void) { return 0; }
int init_pair(short pair, short fg, short bg) {
    if (pair > 0 && pair < 256) {
        g_state.pairs[pair].fg = fg;
        g_state.pairs[pair].bg = bg;
    }
    return 0;
}

int attron(int attrs) {
    if (attrs & 0xFF00) {
        g_state.activePair = static_cast<short>((attrs >> 8) & 0xFF);
    }
    g_state.activeStyle |= (attrs & 0xFF);
    return 0;
}

int attroff(int attrs) {
    if (attrs & 0xFF00) {
        if (g_state.activePair == static_cast<short>((attrs >> 8) & 0xFF)) {
            g_state.activePair = 0;
        }
    }
    g_state.activeStyle &= ~(attrs & 0xFF);
    return 0;
}

static void write_cell(int y, int x, char ch) {
    if (y < 0 || x < 0 || y >= g_state.lines || x >= g_state.cols) {
        return;
    }
    const size_t index = static_cast<size_t>(y) * static_cast<size_t>(g_state.cols) + static_cast<size_t>(x);
    g_state.cells[index].ch = ch;
    g_state.cells[index].attr = compute_attr(g_state.activePair, g_state.activeStyle);
}

int mvaddch(int y, int x, int ch) {
    write_cell(y, x, static_cast<char>(ch));
    return 0;
}

int mvhline(int y, int x, int ch, int n) {
    for (int i = 0; i < n; ++i) {
        write_cell(y, x + i, static_cast<char>(ch));
    }
    return 0;
}

int mvprintw(int y, int x, const char *fmt, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    for (int i = 0; buffer[i] != '\0' && x + i < g_state.cols; ++i) {
        write_cell(y, x + i, buffer[i]);
    }
    return 0;
}

int erase(void) {
    ensure_buffer_size();
    for (auto &cell : g_state.cells) {
        cell.ch = ' ';
        cell.attr = compute_attr(0, A_NORMAL);
    }
    return 0;
}

int wnoutrefresh(WINDOW *) { return 0; }

int doupdate(void) {
    update_console_size();
    ensure_buffer_size();
    if (g_state.outHandle == INVALID_HANDLE_VALUE) {
        return 0;
    }

    CONSOLE_CURSOR_INFO cursorInfo{};
    cursorInfo.dwSize = 25;
    cursorInfo.bVisible = g_state.cursorVisible ? TRUE : FALSE;
    SetConsoleCursorInfo(g_state.outHandle, &cursorInfo);

    COORD home{};
    home.X = 0;
    home.Y = 0;
    SetConsoleCursorPosition(g_state.outHandle, home);

    WORD lastAttr = 0xFFFF;
    for (int y = 0; y < g_state.lines; ++y) {
        for (int x = 0; x < g_state.cols; ++x) {
            const size_t index = static_cast<size_t>(y) * static_cast<size_t>(g_state.cols) + static_cast<size_t>(x);
            const Cell &cell = g_state.cells[index];
            char ch = cell.ch ? cell.ch : ' ';
            render_cell(y, x, ch, cell.attr, lastAttr);
        }
    }
    SetConsoleTextAttribute(g_state.outHandle, g_state.defaultAttr);
    return 0;
}

int curses_getch(void) {
    if (g_state.nodelayEnabled && !_kbhit()) {
        return -1;
    }

    int ch = _getch();
    if (ch == 0 || ch == 224) {
        int code = _getch();
        switch (code) {
            case 72: return KEY_UP;
            case 80: return KEY_DOWN;
            case 75: return KEY_LEFT;
            case 77: return KEY_RIGHT;
            case 28: return KEY_ENTER;
            default: return code;
        }
    }

    if (ch == '\r') {
        return KEY_ENTER;
    }

    return ch;
}

int endwin(void) {
    if (g_state.outHandle != INVALID_HANDLE_VALUE) {
        SetConsoleTextAttribute(g_state.outHandle, g_state.defaultAttr);
    }
    return 0;
}
