#include "types.h"
#include "user.h"
#include "amd64.h"

int
main(int ac, const char *av[])
{
  u64 t0 = rdtsc();

  int pid = fork(0);
  if (pid < 0) {
    fprintf(1, "time: fork failed\n");
    exit();
  }

  if (pid == 0) {
    exec(av[1], av+1);
    fprintf(1, "time: exec failed\n");
    exit();
  }

  wait();
  u64 t1 = rdtsc();
  fprintf(1, "%lu cycles\n", t1-t0);
  exit();
}
