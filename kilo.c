#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <stdio.h>
#include <unistd.h>
#include <termios.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <stdarg.h>
#include <fcntl.h>

#define CTRL_KEY(c) ((c) & 0x1f)
#define TAB_STOP 8 // configure tab spacing here
#define QUIT_TIMES 3 // configure how times to press quit with unsaved stuff

enum editorKey {
  BACKSPACE = 127,
  ARROW_LEFT = 1000,
  ARROW_RIGHT,
  ARROW_UP,
  ARROW_DOWN,
  DEL_KEY,
  HOME_KEY,
  END_KEY,
  PAGE_UP,
  PAGE_DOWN
};

typedef struct erow {
  char *chars;
  int rsize;
  int size;
  char *render;
} erow;

struct config {
  int cx, cy;
  int rx;
  int rowoff;
  int coloff;
  int screenrows;
  int screencols;
  struct termios original_termios;
  int numrows;
  erow *row;
  char *filename;
  char statusmsg[80];
  time_t statustime;
  int dirty;
};
struct config E;

void setStatusMessage(const char *fmt, ...);


void die (const char *s) {
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 3);

  perror(s);
  exit(1);
}

void exitRawMode() {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.original_termios) == -1) {
    die("tcsetattr");
  }
}

void enterRawMode() {
  if (tcgetattr(STDIN_FILENO, &E.original_termios) == -1) {
    die("tcgetattr");
  }
  atexit(exitRawMode); // when program is done makes sure to make terminal normal
  
  struct termios raw = E.original_termios;

  // disables echo, canoncial mode, and bunch of control mappings
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
  raw.c_oflag &= ~(OPOST);

  raw.c_cflag |= (CS8); // makes it 8 bits per byte incase it wasn't already

  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
    die("tcsetattr");
  }
}

int readKey() {
  int nread;
  char c;

  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1) {
      die("read");
    }
  }

  if (c == '\x1b') {
    char seq[3];

    if (read(STDIN_FILENO, &seq[0], 1) != 1) {
      return '\x1b';
    }
    if (read(STDIN_FILENO, &seq[1], 1) != 1) {
      return '\x1b';
    }

    if (seq[0] == '[') {
      if (seq[1] >= '0' && seq[1] <= '9') {
        if (read(STDIN_FILENO, &seq[2], 1) != 1) {
          return '\x1b';
        }

        if (seq[2] == '~') {
          switch (seq[1]) {
            case '1':
              return HOME_KEY;

            case '3':
              return DEL_KEY;

            case '4':
              return END_KEY;

            case '5':
              return PAGE_UP;

            case '6':
              return PAGE_DOWN;
            
            case '7':
              return HOME_KEY;

            case '8':
              return HOME_KEY;
          }
        }
      } else {
          switch (seq[1]) {
            case 'A':
              return ARROW_UP;

            case 'B':
              return ARROW_DOWN;

            case 'C':
              return ARROW_RIGHT;

            case 'D':
              return ARROW_LEFT;
            
            case 'H':
              return HOME_KEY;

            case 'F':
              return END_KEY;
        }
      }
    } else if (seq[0] == 'O') {
        switch (seq[1]) {
          case 'H':
            return HOME_KEY;

          case 'F':
            return END_KEY;
        }
    }

    return '\x1b';
  } else {
      return c;
  }
}

int getCursorPosition(int *rows, int *cols) {
  char buf[32];
  unsigned int i = 0;

  if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) {
    return -1;
  }

  while (i < sizeof(buf) - 1) {
    if (read(STDIN_FILENO, &buf[i],1) != 1) {
      break;
    }
    if (buf[i] == 'R') {
      break;
    }
    i++;
  }
  buf[i] = '\0';
  
  if (buf[0] != '\x1b' || buf[1] != '[') {
    return -1;
  }
  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) {
    return -1;
  }
  
  return 0;
}

int getWindowSize(int *rows, int *cols) {
  struct winsize ws;
  
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    // incase ioctl doesn't work
    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) {
      return -1;
    }
    return getCursorPosition(rows, cols);
  } else {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }
}

int rowCxToRx (erow *row, int cx) {
  int rx = 0;
  int j;

  for (j = 0; j < cx; j++) {
    if (row->chars[j] == '\t') {
      rx += TAB_STOP - 1 + rx%7;
    }
    rx++;
  }

  return rx;
}

void updateRow (erow *row) {
  int tabs = 0;
  int j;
  for (j = 0; j < row->size; j++) {
    if (row->chars[j] == '\t') tabs++;
  }

  free(row->render);
  row->render = malloc(row->size + TAB_STOP*tabs + 1);

  int index = 0;
  for (j = 0; j < row->size; j++) {
    if (row->chars[j] == '\t') {
      row->render[index++] = ' ';
      
      while (index%TAB_STOP != 0) {
        row->render[index++] = ' ';
      }
    } else {
      row->render[index++] = row->chars[j];
    }
  }
  row->render[index] = '\0';
  row->rsize = index;
}

void insertRow(int at, char *s, size_t len) {
  if (at < 0 || at > E.numrows) return;
  E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
  memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));

  E.row[at].size = len;
  E.row[at].chars = malloc(len + 1);
  memcpy(E.row[at].chars, s, len);
  E.row[at].chars[len] = '\0';

  E.row[at].rsize = 0;
  E.row[at].render = NULL;
  updateRow(&E.row[at]);

  E.numrows++;
  E.dirty = 1;
} 

void freeRow(erow *row) {
  free(row->render);
  free(row->chars);
}

void delRow(int at) {
  if (at < 0 || at >= E.numrows) return;
  freeRow(&E.row[at]);
  memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
  E.numrows--;
  E.dirty--;
}

void rowInsertChar(erow *row, int at, int c) {
  if (at < 0 || at > row->size) at = row->size;

  row->chars = realloc(row->chars, row->size + 2);
  memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
  
  row->size++;
  row->chars[at] = c;
  updateRow(row);
  E.dirty = 1;
}

void rowAppendString(erow *row, char *s, size_t len) {
  row->chars = realloc(row->chars, row->size + len + 1);
  memcpy(&row->chars[row->size], s, len);
  row->size += len;
  row->chars[row->size] = '\0';
  updateRow(row);
  E.dirty = 1;
}

void rowDelChar(erow *row, int at) {
  if (at < 0 || at > row->size) return;

  memmove(&row->chars[at], &row->chars[at + 1], row->size - at);

  row->size--;
  updateRow(row);
  E.dirty = 1;
}


void insertChar(int c) { 
  if (E.cy == E.numrows) {
    insertRow(E.numrows, "", 0);
  }
  rowInsertChar(&E.row[E.cy], E.cx, c);
  E.cx++;
}

void insertNewLine() {
  if (E.cx == 0) {
    insertRow(E.cy, "", 0);
  } else {
    erow *row = &E.row[E.cy];
    insertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);

    row = &E.row[E.cy];
    row->size = E.cx;
    row->chars[row->size] = '\0';
    updateRow(row);
  }
  E.cy++;
  E.cx = 0;
}

void delChar() {
  if (E.cy == E.numrows) return;
  if (E.cy == 0 && E.cx == 0) return;

  if (E.cx > 0) {
    rowDelChar(&E.row[E.cy], E.cx - 1);
    E.cx--;
  } else {
    E.cx = E.row[E.cy - 1].size;
    rowAppendString(&E.row[E.cy - 1], E.row[E.cy].chars, E.row[E.cy].size);
    delRow(E.cy);
    E.cy--;
  }
}


char *rowsToString(int *buflen) {
  int totlen = 0;
  int j;
  for (j = 0; j < E.numrows; j++) {
    totlen += E.row[j].size;
  }
  *buflen = totlen;

  char *buf = malloc(totlen);
  char *p = buf;

  for (j = 0; j < E.numrows; j++) {
    memcpy(p, E.row[j].chars, E.row[j].size);
    p += E.row[j].size;
    *p = '\n';
    p++;
  }

  return buf;
}

void editorOpen(char *filename) {
  free(E.filename);
  E.filename = strdup(filename);

  FILE *fp = fopen(filename, "r");
  if (!fp) die("fopen");

  char *line = NULL;
  size_t linecap = 0;
  ssize_t linelen;

  while ((linelen = getline(&line, &linecap, fp)) != -1) {
    while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r')) {
      linelen--;
    }
    insertRow(E.numrows, line, linelen);
  }

  free(line);
  fclose(fp);
  E.dirty = 0;
}

void editorSave() {
  if (E.filename == NULL) return;

  int len;
  char *buf = rowsToString(&len);

  int fd = open(E.filename, O_CREAT | O_RDWR, 0644);
  if (fd != -1) {
    if (ftruncate(fd, len) != -1) {
      if (write(fd, buf, len) == len) {
        close(fd);
        free(buf);
        setStatusMessage("%d bytes written to disk!", len);
        E.dirty = 0;

        return;
      }
      close(fd);
    }
  }

  free(buf);
  setStatusMessage("Could not save. I/O error: %s", strerror(errno));
}


struct abuf{
  char *b;
  int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len) {
  char *new = realloc(ab->b, ab->len + len);

  if (new == NULL) return;
  memcpy(&new[ab->len], s, len);
  ab->b = new;
  ab->len += len;
}

void abFree(struct abuf *ab) {
  free(ab->b);
}

void moveCursor(int key) {
  erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

  switch (key) {
    case ARROW_UP:
      if (E.cy != 0) {
        E.cy--;
      }
      break;

    case ARROW_LEFT:
      if (E.cx != 0) {
        E.cx--;
      } else if (E.cy >= 0) { // left at edge of line moves to previous line
        E.cy--; 
        E.cx = E.row[E.cy].size;
      }
      break;

    case ARROW_DOWN:
      if (E.cy < E.numrows - 1) {
        E.cy++;
      }
      break;

    case ARROW_RIGHT:
      if (row && E.cx < row->size) {
        E.cx++;
      } else if (E.cy < E.numrows - 1) { // right at edge of line moves to next line
        E.cy++;
        E.cx = 0;
      }
      break;
  }

  // makes sure cursor points to end of line when moving up or down from a long line
  row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
  int rowlen = row ? row->size : 0;
  if (E.cx > rowlen) {
    E.cx = rowlen;
  }
}

void processKeypress() {
  static int quit_times = QUIT_TIMES;

  int c = readKey();

  switch (c) {
    case '\r':
      insertNewLine();
      break;
    
    case CTRL_KEY('q') :
      if (E.dirty && quit_times > 0) {
        setStatusMessage("Warning file has unsaved changes! Press Ctrl-q %d more times to quit", quit_times);
        quit_times--;
        return;
      }

      write(STDOUT_FILENO, "\x1b[2J", 4);
      write(STDOUT_FILENO, "\x1b[H", 3);

      exit(0);
      break;

    case CTRL_KEY('s'):
      editorSave();
      break;

    case HOME_KEY:
      E.cx = 0;
      break;

    case END_KEY:
      if (E.cy < E.numrows) {
        E.cx = E.row[E.cy].size;
      }
      break;

    case BACKSPACE:
    case DEL_KEY:
    case CTRL_KEY('h'):
      if (c == DEL_KEY) moveCursor(ARROW_RIGHT);
      delChar();
      break;
    
    case PAGE_UP:
    case PAGE_DOWN:
      {
        if (c == PAGE_UP) {
          E.cy = E.rowoff;
        } else {
          E.cy = E.rowoff + E.screenrows - 1;

          if (E.cy > E.numrows) E.cy = E.numrows - 1;
        }

        int times = E.screenrows;

        while (times--) {
          moveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
        }
      }
      break;
      
    case ARROW_UP:
    case ARROW_LEFT:
    case ARROW_DOWN:
    case ARROW_RIGHT:
      moveCursor(c);
      break;

    case CTRL_KEY('l'):
    case '\x1b':
      break;

    default:
      insertChar(c);
      break;
  }

  quit_times = QUIT_TIMES;
}


void editorScroll() {
  E.rx = 0;
  // make sure on real line
  if (E.cy < E.numrows) {
    E.rx = rowCxToRx(&E.row[E.cy], E.cx);
  }
  

  if (E.cy < E.rowoff) {
    E.rowoff = E.cy;
  }
  if (E.cy >= E.rowoff + E.screenrows) {
    E.rowoff = E.cy - E.screenrows + 1;
  }
  if (E.rx < E.coloff) {
    E.coloff = E.rx;
  }
  if (E.rx >= E.coloff +E.screencols) {
    E.coloff = E.rx - E.screencols + 1;
  }
}

void drawRows(struct abuf *ab) {
  for (int y = 0; y < E.screenrows; y++) {
    int filerow = y + E.rowoff;
    if (filerow >= E.numrows) {
      if (E.numrows == 0 && y == E.screenrows/4) {
        char welcome[80];

        int welcomelen = snprintf(welcome, sizeof(welcome), "Kilo editor");
        if (welcomelen > E.screencols) {
          welcomelen = E.screencols;
        }

        int padding = (E.screencols - welcomelen) / 2;
        if (padding) {
          abAppend(ab, "~", 1);
        }
        while (padding--) {
          abAppend(ab, " ", 1);
        }

        abAppend(ab, welcome, welcomelen);
      } else {
        abAppend(ab, "~", 1);
      }
    } else {
      int len = E.row[filerow].rsize - E.coloff;
      if (len < 0) {len = 0;}
      if (len > E.screencols) {len = E.screencols;}
      abAppend(ab, &E.row[filerow].render[E.coloff], len);
    }
      
    abAppend(ab, "\x1b[K", 3);
    abAppend(ab, "\r\n", 2);
  }
}

void drawStatusBar(struct abuf *ab) {
  abAppend(ab, "\x1b[7m", 4); // inverts colour
  char status[80], rstatus[80];

  int len = snprintf(status, sizeof(status), "%.20s - %d lines %s", E.filename ? E.filename : "No Name", E.numrows, E.dirty ? "(modified)" : "");
  if (len > E.screencols) len = E.screencols;
  abAppend(ab, status, len);
  int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", E.cy + 1, E.numrows);

  while (len < E.screencols) {
    if (E.screencols - len == rlen) {
      abAppend(ab, rstatus, rlen);
      break;
    } else {
      abAppend(ab, " ", 1);
      len++;
    }
  }

  abAppend(ab, "\x1b[m", 3); // normal colour
  abAppend(ab, "\r\n", 2);
}

void drawMessageBar (struct abuf *ab) {
  abAppend(ab, "\x1b[K", 3);
  int msglen = strlen(E.statusmsg);
  if (msglen > E.screencols) msglen = E.screencols;

  if (msglen && time(NULL) - E.statustime < 5) {
    abAppend(ab, E.statusmsg, msglen);
  }
}

void refreshScreen() {
  editorScroll();

  struct abuf ab = ABUF_INIT;

  abAppend(&ab, "\x1b[?25l", 6); // hide cursor
  abAppend(&ab, "\x1b[H", 3); // cursor top left

  drawRows(&ab);
  drawStatusBar(&ab);
  drawMessageBar(&ab);

  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, (E.rx - E.coloff) + 1);
  abAppend(&ab, buf, strlen(buf));

  abAppend(&ab, "\x1b[?25h", 6); // unhide cursor

  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}

void setStatusMessage(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
  va_end(ap);
  E.statustime = time(NULL);
}

void initEditor() {
  E.cx = 0;
  E.cy = 0;
  E.rx = 0;
  E.rowoff = 0;
  E.coloff = 0;
  E.numrows = 0;
  E.row = NULL;
  E.filename = NULL;
  E.statusmsg[0] = '\0';
  E.statustime = 0;
  E.dirty = 0;

  if (getWindowSize(&E.screenrows, &E.screencols) == -1) {
    die("getWindowSize");
  }
  E.screenrows -= 2;
}

int main(int argc, char *argv[]) {
  enterRawMode();
  initEditor();
  if (argc >= 2) {
    editorOpen(argv[1]);
  }

  setStatusMessage("Ctrl-q = Quit  |  Ctrl-s = Save");

  while (1) {
      refreshScreen();
      processKeypress();
    }
  return 0;
}

