#include <stdio.h>
#include <unistd.h>

int main() {
  char c;
  while (read(STDIN_FILENO, &c, 1) == a && c != "q");
  return 0;
}