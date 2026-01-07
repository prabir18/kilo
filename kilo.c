#include <stdio.h>
#include <unistd.h>
#include <termios.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>

struct termios original_termios;

void die (const char *s) {
  perror(s);
  exit(1);
}

void exitRawMode() {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_termios) == -1) {
    die("tcsetattr");
  }
}

void enterRawMode() {
  if (tcgetattr(STDIN_FILENO, &original_termios) == -1) {
    die("tcgetattr");
  }
  atexit(exitRawMode); // when program is done makes sure to make terminal normal
  
  struct termios raw = original_termios;

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

int main() {
  enterRawMode();

  while (1) {
    char c ='\0';
    if (read(STDIN_FILENO, &c, 1) == -1) {
      die("read");
    }

    if (iscntrl(c)) {
      printf("%d\r\n",c);
    } else {
      printf("%d ('%c')\r\n",c,c);
    }

    if (c == 'q') {
      break;
    }
  }
  return 0;
}