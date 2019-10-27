#include <stdio.h>

#include "user.h"

int
main(int argc, char *argv[])
{
  printf("has_secrets: %d\n", has_secrets());
  return 0;
}
