#include "types.h"
#include "user.h"
#include "fs.h"
#include "traps.h"
#include "pthread.h"
#include "rnd.hh"

#include <fcntl.h>
#include <sys/mman.h>

#include <utility>

char buf[2048];
char name[3];
const char *echoargv[] = { "echo", "ALL", "TESTS", "PASSED", 0 };
static int stdout = 1;

// simple file system tests

void
opentest(void)
{
  int fd;

  fprintf(stdout, "open test\n");
  fd = open("echo", 0);
  if(fd < 0){
    fprintf(stdout, "open echo failed!\n");
    exit();
  }
  close(fd);
  fd = open("doesnotexist", 0);
  if(fd >= 0){
    fprintf(stdout, "open doesnotexist succeeded!\n");
    exit();
  }
  fprintf(stdout, "open test ok\n");
}

void
writetest(void)
{
  int fd;
  int i;

  fprintf(stdout, "small file test\n");
  fd = open("small", O_CREAT|O_RDWR, 0666);
  if(fd >= 0){
    fprintf(stdout, "creat small succeeded; ok\n");
  } else {
    fprintf(stdout, "error: creat small failed!\n");
    exit();
  }
  for(i = 0; i < 100; i++){
    if(write(fd, "aaaaaaaaaa", 10) != 10){
      fprintf(stdout, "error: write aa %d new file failed\n", i);
      exit();
    }
    if(write(fd, "bbbbbbbbbb", 10) != 10){
      fprintf(stdout, "error: write bb %d new file failed\n", i);
      exit();
    }
  }
  fprintf(stdout, "writes ok\n");
  close(fd);
  fd = open("small", O_RDONLY);
  if(fd >= 0){
    fprintf(stdout, "open small succeeded ok\n");
  } else {
    fprintf(stdout, "error: open small failed!\n");
    exit();
  }
  i = read(fd, buf, 2000);
  if(i == 2000){
    fprintf(stdout, "read succeeded ok\n");
  } else {
    fprintf(stdout, "read failed\n");
    exit();
  }
  close(fd);

  if(unlink("small") < 0){
    fprintf(stdout, "unlink small failed\n");
    exit();
  }
  fprintf(stdout, "small file test ok\n");
}

void
writetest1(void)
{
  int fd, n;

  fprintf(stdout, "big files test\n");

  fd = open("big", O_CREAT|O_RDWR, 0666);
  if(fd < 0){
    fprintf(stdout, "error: creat big failed!\n");
    exit();
  }

  for(u32 i = 0; i < MAXFILE; i++){
    ((int*)buf)[0] = i;
    if(write(fd, buf, 512) != 512){
      fprintf(stdout, "error: write big file failed\n", i);
      exit();
    }
  }

  close(fd);

  fd = open("big", O_RDONLY);
  if(fd < 0){
    fprintf(stdout, "error: open big failed!\n");
    exit();
  }

  n = 0;
  for(;;){
    u32 i = read(fd, buf, 512);
    if(i == 0){
      if(n == MAXFILE - 1){
        fprintf(stdout, "read only %d blocks from big", n);
        exit();
      }
      break;
    } else if(i != 512){
      fprintf(stdout, "read failed %d\n", i);
      exit();
    }
    if(((int*)buf)[0] != n){
      fprintf(stdout, "read content of block %d is %d\n",
             n, ((int*)buf)[0]);
      exit();
    }
    n++;
  }
  close(fd);
  if(unlink("big") < 0){
    fprintf(stdout, "unlink big failed\n");
    exit();
  }
  fprintf(stdout, "big files ok\n");
}

void
createtest(void)
{
  int i, fd;

  fprintf(stdout, "many creates, followed by unlink test\n");

  name[0] = 'a';
  name[2] = '\0';
  for(i = 0; i < 52; i++){
    name[1] = '0' + i;
    fd = open(name, O_CREAT|O_RDWR, 0666);
    close(fd);
  }
  name[0] = 'a';
  name[2] = '\0';
  for(i = 0; i < 52; i++){
    name[1] = '0' + i;
    unlink(name);
  }
  fprintf(stdout, "many creates, followed by unlink; ok\n");
}

void dirtest(void)
{
  fprintf(stdout, "mkdir test\n");

  if(mkdir("dir0", 0777) < 0){
    fprintf(stdout, "mkdir failed\n");
    exit();
  }

  if(chdir("dir0") < 0){
    fprintf(stdout, "chdir dir0 failed\n");
    exit();
  }

  if(chdir("..") < 0){
    fprintf(stdout, "chdir .. failed\n");
    exit();
  }

  if(unlink("dir0") < 0){
    fprintf(stdout, "unlink dir0 failed\n");
    exit();
  }
  fprintf(stdout, "mkdir test\n");
}

void
exectest(void)
{
  fprintf(stdout, "exec test\n");
  if(exec("echo", echoargv) < 0){
    fprintf(stdout, "exec echo failed\n");
    exit();
  }
}

// simple fork and pipe read/write

void
pipe1(void)
{
  int fds[2], pid;
  int seq, i, n, cc, total;

  if(pipe(fds, 0) != 0){
    fprintf(1, "pipe() failed\n");
    exit();
  }
  pid = fork(0);
  seq = 0;
  if(pid == 0){
    close(fds[0]);
    for(n = 0; n < 5; n++){
      for(i = 0; i < 1033; i++)
        buf[i] = seq++;
      if(write(fds[1], buf, 1033) != 1033){
        fprintf(1, "pipe1 oops 1\n");
        exit();
      }
    }
    exit();
  } else if(pid > 0){
    close(fds[1]);
    total = 0;
    cc = 1;
    while((n = read(fds[0], buf, cc)) > 0){
      for(i = 0; i < n; i++){
        if((buf[i] & 0xff) != (seq++ & 0xff)){
          fprintf(1, "pipe1 oops 2\n");
          return;
        }
      }
      total += n;
      cc = cc * 2;
      if(cc > sizeof(buf))
        cc = sizeof(buf);
    }
    if(total != 5 * 1033)
      fprintf(1, "pipe1 oops 3 total %d\n", total);
    close(fds[0]);
    wait(-1);
  } else {
    fprintf(1, "fork(0) failed\n");
    exit();
  }
  fprintf(1, "pipe1 ok\n");
}

// meant to be run w/ at most two CPUs
void
preempt(void)
{
  int pid1, pid2, pid3;
  int pfds[2];

  fprintf(1, "preempt: ");
  pid1 = fork(0);
  if(pid1 == 0)
    for(;;)
      ;

  pid2 = fork(0);
  if(pid2 == 0)
    for(;;)
      ;

  pipe(pfds, 0);
  pid3 = fork(0);
  if(pid3 == 0){
    close(pfds[0]);
    if(write(pfds[1], "x", 1) != 1)
      fprintf(1, "preempt write error");
    close(pfds[1]);
    for(;;)
      ;
  }

  close(pfds[1]);
  if(read(pfds[0], buf, sizeof(buf)) != 1){
    fprintf(1, "preempt read error");
    return;
  }
  close(pfds[0]);
  fprintf(1, "kill... ");
  kill(pid1);
  kill(pid2);
  kill(pid3);
  fprintf(1, "wait... ");
  wait(-1);
  wait(-1);
  wait(-1);
  fprintf(1, "preempt ok\n");
}

// try to find any races between exit and wait
void
exitwait(void)
{
  int i, pid;

  for(i = 0; i < 100; i++){
    pid = fork(0);
    if(pid < 0){
      fprintf(1, "fork failed\n");
      return;
    }
    if(pid){
      if(wait(-1) != pid){
        fprintf(1, "wait wrong pid\n");
        return;
      }
    } else {
      exit();
    }
  }
  fprintf(1, "exitwait ok\n");
}

void
mem(void)
{
  void *m1, *m2;
  int pid, ppid;

  fprintf(1, "mem test\n");
  ppid = getpid();
  if((pid = fork(0)) == 0){
    m1 = 0;
    while((m2 = malloc(10001)) != 0){
      *(char**)m2 = (char*) m1;
      m1 = m2;
    }
    while(m1){
      m2 = *(char**)m1;
      free(m1);
      m1 = m2;
    }
    m1 = malloc(1024*20);
    if(m1 == 0){
      fprintf(1, "couldn't allocate mem?!!\n");
      kill(ppid);
      exit();
    }
    free(m1);
    fprintf(1, "mem ok\n");
    exit();
  } else {
    wait(-1);
  }
}

// More file system tests

// two processes write to the same file descriptor
// is the offset shared? does inode locking work?
void
sharedfd(void)
{
  int fd, pid, i, n, nc, np;
  char buf[10];

  unlink("sharedfd");
  fd = open("sharedfd", O_CREAT|O_RDWR, 0666);
  if(fd < 0){
    fprintf(1, "fstests: cannot open sharedfd for writing");
    return;
  }
  pid = fork(0);
  memset(buf, pid==0?'c':'p', sizeof(buf));
  for(i = 0; i < 1000; i++){
    if(write(fd, buf, sizeof(buf)) != sizeof(buf)){
      fprintf(1, "fstests: write sharedfd failed\n");
      break;
    }
  }
  if(pid == 0)
    exit();
  else
    wait(-1);
  close(fd);
  fd = open("sharedfd", 0);
  if(fd < 0){
    fprintf(1, "fstests: cannot open sharedfd for reading\n");
    return;
  }
  nc = np = 0;
  while((n = read(fd, buf, sizeof(buf))) > 0){
    for(i = 0; i < sizeof(buf); i++){
      if(buf[i] == 'c')
        nc++;
      if(buf[i] == 'p')
        np++;
    }
  }
  close(fd);
  unlink("sharedfd");
  if(nc == 10000 && np == 10000)
    fprintf(1, "sharedfd ok\n");
  else
    fprintf(1, "sharedfd oops %d %d\n", nc, np);
}

// two processes write two different files at the same
// time, to test block allocation.
void
twofiles(void)
{
  int fd, pid, i, j, n, total;
  const char *fname;

  fprintf(1, "twofiles test\n");

  unlink("f1");
  unlink("f2");

  pid = fork(0);
  if(pid < 0){
    fprintf(1, "fork failed\n");
    return;
  }

  fname = pid ? "f1" : "f2";
  fd = open(fname, O_CREAT | O_RDWR, 0666);
  if(fd < 0){
    fprintf(1, "create failed\n");
    exit();
  }

  memset(buf, pid?'p':'c', 512);
  for(i = 0; i < 12; i++){
    if((n = write(fd, buf, 500)) != 500){
      fprintf(1, "write failed %d\n", n);
      exit();
    }
  }
  close(fd);
  if(pid)
    wait(-1);
  else
    exit();

  for(i = 0; i < 2; i++){
    fd = open(i?"f1":"f2", 0);
    total = 0;
    while((n = read(fd, buf, sizeof(buf))) > 0){
      for(j = 0; j < n; j++){
        if(buf[j] != (i?'p':'c')){
          fprintf(1, "wrong char\n");
          exit();
        }
      }
      total += n;
    }
    close(fd);
    if(total != 12*500){
      fprintf(1, "wrong length %d\n", total);
      exit();
    }
  }

  unlink("f1");
  unlink("f2");

  fprintf(1, "twofiles ok\n");
}

// two processes create and delete different files in same directory
void
createdelete(void)
{
  enum { N = 20 };
  int pid, i, fd;
  char name[32];

  fprintf(1, "createdelete test\n");
  pid = fork(0);
  if(pid < 0){
    fprintf(1, "fork failed\n");
    exit();
  }

  name[0] = pid ? 'p' : 'c';
  name[2] = '\0';
  for(i = 0; i < N; i++){
    name[1] = '0' + i;
    fd = open(name, O_CREAT | O_RDWR, 0666);
    if(fd < 0){
      fprintf(1, "create failed\n");
      exit();
    }
    close(fd);
    if(i > 0 && (i % 2 ) == 0){
      name[1] = '0' + (i / 2);
      if(unlink(name) < 0){
        fprintf(1, "unlink failed\n");
        exit();
      }
    }
  }

  if(pid==0)
    exit();
  else
    wait(-1);

  for(i = 0; i < N; i++){
    name[0] = 'p';
    name[1] = '0' + i;
    fd = open(name, 0);
    if((i == 0 || i >= N/2) && fd < 0){
      fprintf(1, "oops createdelete %s didn't exist\n", name);
      exit();
    } else if((i >= 1 && i < N/2) && fd >= 0){
      fprintf(1, "oops createdelete %s did exist\n", name);
      exit();
    }
    if(fd >= 0)
      close(fd);

    name[0] = 'c';
    name[1] = '0' + i;
    fd = open(name, 0);
    if((i == 0 || i >= N/2) && fd < 0){
      fprintf(1, "oops createdelete %s didn't exist\n", name);
      exit();
    } else if((i >= 1 && i < N/2) && fd >= 0){
      fprintf(1, "oops createdelete %s did exist\n", name);
      exit();
    }
    if(fd >= 0)
      close(fd);
  }

  for(i = 0; i < N; i++){
    name[0] = 'p';
    name[1] = '0' + i;
    unlink(name);
    name[0] = 'c';
    unlink(name);
  }

  fprintf(1, "createdelete ok\n");
}

// can I unlink a file and still read it?
void
unlinkread(void)
{
  int fd, fd1;

  fprintf(1, "unlinkread test\n");
  fd = open("unlinkread", O_CREAT | O_RDWR, 0666);
  if(fd < 0){
    fprintf(1, "create unlinkread failed\n");
    exit();
  }
  write(fd, "hello", 5);
  close(fd);

  fd = open("unlinkread", O_RDWR);
  if(fd < 0){
    fprintf(1, "open unlinkread failed\n");
    exit();
  }
  if(unlink("unlinkread") != 0){
    fprintf(1, "unlink unlinkread failed\n");
    exit();
  }

  fd1 = open("unlinkread", O_CREAT | O_RDWR, 0666);
  write(fd1, "yyy", 3);
  close(fd1);

  if(read(fd, buf, sizeof(buf)) != 5){
    fprintf(1, "unlinkread read failed");
    exit();
  }
  if(buf[0] != 'h'){
    fprintf(1, "unlinkread wrong data\n");
    exit();
  }
  if(write(fd, buf, 10) != 10){
    fprintf(1, "unlinkread write failed\n");
    exit();
  }
  close(fd);
  unlink("unlinkread");
  fprintf(1, "unlinkread ok\n");
}

void
linktest(void)
{
  int fd;

  fprintf(1, "linktest\n");

  unlink("lf1");
  unlink("lf2");

  fd = open("lf1", O_CREAT|O_RDWR, 0666);
  if(fd < 0){
    fprintf(1, "create lf1 failed\n");
    exit();
  }
  if(write(fd, "hello", 5) != 5){
    fprintf(1, "write lf1 failed\n");
    exit();
  }
  close(fd);

  if(link("lf1", "lf2") < 0){
    fprintf(1, "link lf1 lf2 failed\n");
    exit();
  }
  unlink("lf1");

  if(open("lf1", 0) >= 0){
    fprintf(1, "unlinked lf1 but it is still there!\n");
    exit();
  }

  fd = open("lf2", 0);
  if(fd < 0){
    fprintf(1, "open lf2 failed\n");
    exit();
  }
  if(read(fd, buf, sizeof(buf)) != 5){
    fprintf(1, "read lf2 failed\n");
    exit();
  }
  close(fd);

  if(link("lf2", "lf2") >= 0){
    fprintf(1, "link lf2 lf2 succeeded! oops\n");
    exit();
  }

  unlink("lf2");
  if(link("lf2", "lf1") >= 0){
    fprintf(1, "link non-existant succeeded! oops\n");
    exit();
  }

  if(link(".", "lf1") >= 0){
    fprintf(1, "link . lf1 succeeded! oops\n");
    exit();
  }

  fprintf(1, "linktest ok\n");
}

// test concurrent create and unlink of the same file
void
concreate(void)
{
  char file[3];
  int i, pid, n, fd;
  char fa[40];
  struct {
    u16 inum;
    char name[14];
  } de;

  fprintf(1, "concreate test\n");
  file[0] = 'C';
  file[2] = '\0';
  for(i = 0; i < 40; i++){
    file[1] = '0' + i;
    unlink(file);
    pid = fork(0);
    if(pid && (i % 3) == 1){
      link("C0", file);
    } else if(pid == 0 && (i % 5) == 1){
      link("C0", file);
    } else {
      fd = open(file, O_CREAT | O_RDWR, 0666);
      if(fd < 0){
        fprintf(1, "concreate create %s failed\n", file);
        exit();
      }
      close(fd);
    }
    if(pid == 0)
      exit();
    else
      wait(-1);
  }

  memset(fa, 0, sizeof(fa));
  fd = open(".", 0);
  n = 0;
  while(read(fd, &de, sizeof(de)) > 0){
    if(de.inum == 0)
      continue;
    if(de.name[0] == 'C' && de.name[2] == '\0'){
      i = de.name[1] - '0';
      if(i < 0 || i >= sizeof(fa)){
        fprintf(1, "concreate weird file %s\n", de.name);
        exit();
      }
      if(fa[i]){
        fprintf(1, "concreate duplicate file %s\n", de.name);
        exit();
      }
      fa[i] = 1;
      n++;
    }
  }
  close(fd);

  if(n != 40){
    fprintf(1, "concreate not enough files in directory listing\n");
    exit();
  }

  for(i = 0; i < 40; i++){
    file[1] = '0' + i;
    pid = fork(0);
    if(pid < 0){
      fprintf(1, "fork failed\n");
      exit();
    }
    if(((i % 3) == 0 && pid == 0) ||
       ((i % 3) == 1 && pid != 0)){
      fd = open(file, 0);
      close(fd);
    } else {
      unlink(file);
    }
    if(pid == 0)
      exit();
    else
      wait(-1);
  }

  fprintf(1, "concreate ok\n");
}

// directory that uses indirect blocks
void
bigdir(void)
{
  int i, fd;
  char name[10];

  fprintf(1, "bigdir test\n");
  unlink("bd");

  fd = open("bd", O_CREAT, 0666);
  if(fd < 0){
    fprintf(1, "bigdir create failed\n");
    exit();
  }
  close(fd);

  for(i = 0; i < 500; i++){
    name[0] = 'x';
    name[1] = '0' + (i / 64);
    name[2] = '0' + (i % 64);
    name[3] = '\0';
    if(link("bd", name) != 0){
      fprintf(1, "bigdir link failed\n");
      exit();
    }
  }

  unlink("bd");
  for(i = 0; i < 500; i++){
    name[0] = 'x';
    name[1] = '0' + (i / 64);
    name[2] = '0' + (i % 64);
    name[3] = '\0';
    if(unlink(name) != 0){
      fprintf(1, "bigdir unlink failed");
      exit();
    }
  }

  fprintf(1, "bigdir ok\n");
}

void
subdir(void)
{
  int fd, cc;

  fprintf(1, "subdir test\n");

  unlink("ff");
  if(mkdir("dd", 0777) != 0){
    fprintf(1, "subdir mkdir dd failed\n");
    exit();
  }

  fd = open("dd/ff", O_CREAT | O_RDWR, 0666);
  if(fd < 0){
    fprintf(1, "create dd/ff failed\n");
    exit();
  }
  write(fd, "ff", 2);
  close(fd);
  
  if(unlink("dd") >= 0){
    fprintf(1, "unlink dd (non-empty dir) succeeded!\n");
    exit();
  }

  if(mkdir("/dd/dd", 0777) != 0){
    fprintf(1, "subdir mkdir dd/dd failed\n");
    exit();
  }

  fd = open("dd/dd/ff", O_CREAT | O_RDWR, 0666);
  if(fd < 0){
    fprintf(1, "create dd/dd/ff failed\n");
    exit();
  }
  write(fd, "FF", 2);
  close(fd);

  fd = open("dd/dd/../ff", 0);
  if(fd < 0){
    fprintf(1, "open dd/dd/../ff failed\n");
    exit();
  }
  cc = read(fd, buf, sizeof(buf));
  if(cc != 2 || buf[0] != 'f'){
    fprintf(1, "dd/dd/../ff wrong content\n");
    exit();
  }
  close(fd);

  if(link("dd/dd/ff", "dd/dd/ffff") != 0){
    fprintf(1, "link dd/dd/ff dd/dd/ffff failed\n");
    exit();
  }

  if(unlink("dd/dd/ff") != 0){
    fprintf(1, "unlink dd/dd/ff failed\n");
    exit();
  }
  if(open("dd/dd/ff", O_RDONLY) >= 0){
    fprintf(1, "open (unlinked) dd/dd/ff succeeded\n");
    exit();
  }

  if(chdir("dd") != 0){
    fprintf(1, "chdir dd failed\n");
    exit();
  }
  if(chdir("dd/../../dd") != 0){
    fprintf(1, "chdir dd/../../dd failed\n");
    exit();
  }
  if(chdir("dd/../../../dd") != 0){
    fprintf(1, "chdir dd/../../dd failed\n");
    exit();
  }
  if(chdir("./..") != 0){
    fprintf(1, "chdir ./.. failed\n");
    exit();
  }

  fd = open("dd/dd/ffff", 0);
  if(fd < 0){
    fprintf(1, "open dd/dd/ffff failed\n");
    exit();
  }
  if(read(fd, buf, sizeof(buf)) != 2){
    fprintf(1, "read dd/dd/ffff wrong len\n");
    exit();
  }
  close(fd);

  if(open("dd/dd/ff", O_RDONLY) >= 0){
    fprintf(1, "open (unlinked) dd/dd/ff succeeded!\n");
    exit();
  }

  if(open("dd/ff/ff", O_CREAT|O_RDWR, 0666) >= 0){
    fprintf(1, "create dd/ff/ff succeeded!\n");
    exit();
  }
  if(open("dd/xx/ff", O_CREAT|O_RDWR, 0666) >= 0){
    fprintf(1, "create dd/xx/ff succeeded!\n");
    exit();
  }
  if(open("dd", O_CREAT, 0666) >= 0){
    fprintf(1, "create dd succeeded!\n");
    exit();
  }
  if(open("dd", O_RDWR) >= 0){
    fprintf(1, "open dd rdwr succeeded!\n");
    exit();
  }
  if(open("dd", O_WRONLY) >= 0){
    fprintf(1, "open dd wronly succeeded!\n");
    exit();
  }
  if(link("dd/ff/ff", "dd/dd/xx") == 0){
    fprintf(1, "link dd/ff/ff dd/dd/xx succeeded!\n");
    exit();
  }
  if(link("dd/xx/ff", "dd/dd/xx") == 0){
    fprintf(1, "link dd/xx/ff dd/dd/xx succeeded!\n");
    exit();
  }
  if(link("dd/ff", "dd/dd/ffff") == 0){
    fprintf(1, "link dd/ff dd/dd/ffff succeeded!\n");
    exit();
  }
  if(mkdir("dd/ff/ff", 0777) == 0){
    fprintf(1, "mkdir dd/ff/ff succeeded!\n");
    exit();
  }
  if(mkdir("dd/xx/ff", 0777) == 0){
    fprintf(1, "mkdir dd/xx/ff succeeded!\n");
    exit();
  }
  if(mkdir("dd/dd/ffff", 0777) == 0){
    fprintf(1, "mkdir dd/dd/ffff succeeded!\n");
    exit();
  }
  if(unlink("dd/xx/ff") == 0){
    fprintf(1, "unlink dd/xx/ff succeeded!\n");
    exit();
  }
  if(unlink("dd/ff/ff") == 0){
    fprintf(1, "unlink dd/ff/ff succeeded!\n");
    exit();
  }
  if(chdir("dd/ff") == 0){
    fprintf(1, "chdir dd/ff succeeded!\n");
    exit();
  }
  if(chdir("dd/xx") == 0){
    fprintf(1, "chdir dd/xx succeeded!\n");
    exit();
  }

  if(unlink("dd/dd/ffff") != 0){
    fprintf(1, "unlink dd/dd/ff failed\n");
    exit();
  }
  if(unlink("dd/ff") != 0){
    fprintf(1, "unlink dd/ff failed\n");
    exit();
  }
  if(unlink("dd") == 0){
    fprintf(1, "unlink non-empty dd succeeded!\n");
    exit();
  }
  if(unlink("dd/dd") < 0){
    fprintf(1, "unlink dd/dd failed\n");
    exit();
  }
  if(unlink("dd") < 0){
    fprintf(1, "unlink dd failed\n");
    exit();
  }

  fprintf(1, "subdir ok\n");
}

void
bigfile(void)
{
  int fd, i, total, cc;

  fprintf(1, "bigfile test\n");

  unlink("bigfile");
  fd = open("bigfile", O_CREAT | O_RDWR, 0666);
  if(fd < 0){
    fprintf(1, "cannot create bigfile");
    exit();
  }
  for(i = 0; i < 20; i++){
    memset(buf, i, 600);
    if(write(fd, buf, 600) != 600){
      fprintf(1, "write bigfile failed\n");
      exit();
    }
  }
  close(fd);

  fd = open("bigfile", 0);
  if(fd < 0){
    fprintf(1, "cannot open bigfile\n");
    exit();
  }
  total = 0;
  for(i = 0; ; i++){
    cc = read(fd, buf, 300);
    if(cc < 0){
      fprintf(1, "read bigfile failed\n");
      exit();
    }
    if(cc == 0)
      break;
    if(cc != 300){
      fprintf(1, "short read bigfile\n");
      exit();
    }
    if(buf[0] != i/2 || buf[299] != i/2){
      fprintf(1, "read bigfile wrong data\n");
      exit();
    }
    total += cc;
  }
  close(fd);
  if(total != 20*600){
    fprintf(1, "read bigfile wrong total\n");
    exit();
  }
  unlink("bigfile");

  fprintf(1, "bigfile test ok\n");
}

void
thirteen(void)
{
  int fd;

  // DIRSIZ is 14.
  fprintf(1, "thirteen test\n");

  if(mkdir("1234567890123", 0777) != 0){
    fprintf(1, "mkdir 1234567890123 failed\n");
    exit();
  }
  if(mkdir("1234567890123/1234567890123", 0777) != 0){
    fprintf(1, "mkdir 1234567890123/1234567890123 failed\n");
    exit();
  }
  fd = open("1234567890123/1234567890123/1234567890123", O_CREAT, 0666);
  if(fd < 0){
    fprintf(1, "create 1234567890123/1234567890123/1234567890123 failed\n");
    exit();
  }
  close(fd);
  fd = open("1234567890123/1234567890123/1234567890123", 0);
  if(fd < 0){
    fprintf(1, "open 1234567890123/1234567890123/1234567890123 failed\n");
    exit();
  }
  close(fd);

  if(mkdir("1234567890123/1234567890123", 0777) == 0){
    fprintf(1, "mkdir 1234567890123/1234567890123 succeeded!\n");
    exit();
  }
  if(mkdir("1234567890123/1234567890123", 0777) == 0){
    fprintf(1, "mkdir 1234567890123/1234567890123 succeeded!\n");
    exit();
  }

  fprintf(1, "thirteen ok\n");
}

void
longname(void)
{
  fprintf(stdout, "longname\n");
  for (int i = 0; i < 100; i++) {
    if (open("123456789012345", O_CREAT, 0666) != -1) {
      fprintf(stdout, "open 123456789012345, O_CREAT succeeded!\n");
      exit();
    }  
    if (mkdir("123456789012345", 0777) != -1) {
      fprintf(stdout, "mkdir 123456789012345 succeeded!\n");
      exit();
    }
  }
  fprintf(stdout, "longname ok\n");
}

void
rmdot(void)
{
  fprintf(1, "rmdot test\n");
  if(mkdir("dots", 0777) != 0){
    fprintf(1, "mkdir dots failed\n");
    exit();
  }
  if(chdir("dots") != 0){
    fprintf(1, "chdir dots failed\n");
    exit();
  }
  if(unlink(".") == 0){
    fprintf(1, "rm . worked!\n");
    exit();
  }
  if(unlink("..") == 0){
    fprintf(1, "rm .. worked!\n");
    exit();
  }
  if(chdir("/") != 0){
    fprintf(1, "chdir / failed\n");
    exit();
  }
  if(unlink("dots/.") == 0){
    fprintf(1, "unlink dots/. worked!\n");
    exit();
  }
  if(unlink("dots/..") == 0){
    fprintf(1, "unlink dots/.. worked!\n");
    exit();
  }
  if(unlink("dots") != 0){
    fprintf(1, "unlink dots failed!\n");
    exit();
  }
  fprintf(1, "rmdot ok\n");
}

void
dirfile(void)
{
  int fd;

  fprintf(1, "dir vs file\n");

  fd = open("dirfile", O_CREAT, 0666);
  if(fd < 0){
    fprintf(1, "create dirfile failed\n");
    exit();
  }
  close(fd);
  if(chdir("dirfile") == 0){
    fprintf(1, "chdir dirfile succeeded!\n");
    exit();
  }
  fd = open("dirfile/xx", 0);
  if(fd >= 0){
    fprintf(1, "create dirfile/xx succeeded!\n");
    exit();
  }
  fd = open("dirfile/xx", O_CREAT, 0666);
  if(fd >= 0){
    fprintf(1, "create dirfile/xx succeeded!\n");
    exit();
  }
  if(mkdir("dirfile/xx", 0777) == 0){
    fprintf(1, "mkdir dirfile/xx succeeded!\n");
    exit();
  }
  if(unlink("dirfile/xx") == 0){
    fprintf(1, "unlink dirfile/xx succeeded!\n");
    exit();
  }
  if(link("README", "dirfile/xx") == 0){
    fprintf(1, "link to dirfile/xx succeeded!\n");
    exit();
  }
  if(unlink("dirfile") != 0){
    fprintf(1, "unlink dirfile failed!\n");
    exit();
  }

  fd = open(".", O_RDWR);
  if(fd >= 0){
    fprintf(1, "open . for writing succeeded!\n");
    exit();
  }
  fd = open(".", 0);
  if(write(fd, "x", 1) > 0){
    fprintf(1, "write . succeeded!\n");
    exit();
  }
  close(fd);

  fprintf(1, "dir vs file OK\n");
}

// test that iput() is called at the end of _namei()
void
iref(void)
{
  int i, fd;

  fprintf(1, "empty file name\n");

  // the 50 is NINODE
  for(i = 0; i < 50 + 1; i++){
    if(mkdir("irefd", 0777) != 0){
      fprintf(1, "mkdir irefd failed\n");
      exit();
    }
    if(chdir("irefd") != 0){
      fprintf(1, "chdir irefd failed\n");
      exit();
    }

    mkdir("", 0777);
    link("README", "");
    fd = open("", O_CREAT, 0666);
    if(fd >= 0)
      close(fd);
    fd = open("xx", O_CREAT, 0666);
    if(fd >= 0)
      close(fd);
    unlink("xx");
  }

  chdir("/");
  fprintf(1, "empty file name OK\n");
}

// test that fork fails gracefully
// the forktest binary also does this, but it runs out of proc entries first.
// inside the bigger usertests binary, we run out of memory first.
void
forktest(void)
{
  int n, pid;

  fprintf(1, "fork test\n");

  for(n=0; n<1000; n++){
    pid = fork(0);
    if(pid < 0)
      break;
    if(pid == 0)
      exit();
  }
   
  for(; n > 0; n--){
    if(wait(-1) < 0){
      fprintf(1, "wait stopped early\n");
      exit();
    }
  }
  
  if(wait(-1) != -1){
    fprintf(1, "wait got too many\n");
    exit();
  }
  
  fprintf(1, "fork test OK\n");
}

void
memtest(void)
{
#define NMAP 1024
  static void *addr[1024];
  if (setaffinity(0) < 0)
    die("setaffinity err");
  
  for (int i = 0; i < NMAP; i++) {
    // allocate enough memory that a core must steal memory from another pool
    char *p = (char*) mmap(0, 256 * 1024, PROT_READ|PROT_WRITE,
                           MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
      die("%d: map failed");
    addr[i] = p;
    // force allocation of memory
    for (int j = 0; j < 256*1024; j += 4096) {
      p[j] = 1;
    }
  }
  for (int i = 0; i < NMAP; i++) {
    int r = munmap(addr[i], 256*1024);
    if (r < 0)
      die("memtest: unmap failed");
  }
}

void
sbrktest(void)
{
  int fds[2], pid, pids[100];
  char *a, *b, *c, *lastaddr, *oldbrk, *p, scratch;
  uptr amt;

  fprintf(stdout, "sbrk test\n");
  oldbrk = sbrk(0);

  // can one sbrk() less than a page?
  a = sbrk(0);
  int i;
  for(i = 0; i < 5000; i++){
    b = sbrk(1);
    if(b != a){
      fprintf(stdout, "sbrk test failed %d %x %x\n", i, a, b);
      exit();
    }
    *b = 1;
    a = b + 1;
  }
  pid = fork(0);
  if(pid < 0){
    fprintf(stdout, "sbrk test fork failed\n");
    exit();
  }
  c = sbrk(1);
  c = sbrk(1);
  if(c != a + 1){
    fprintf(stdout, "sbrk test failed post-fork\n");
    exit();
  }
  if(pid == 0)
    exit();
  wait(-1);

  // can one allocate the full 640K?
  // less a stack page and an empty page at the top.
  a = sbrk(0);
  amt = (632 * 1024) - (uptr)a;
  p = sbrk(amt);
  if(p != a){
    fprintf(stdout, "sbrk test failed 632K test, p %x a %x\n", p, a);
    exit();
  }
  lastaddr = p - 1;
  *lastaddr = 99;

#if 0
  // is one forbidden from allocating more than 632K?
  c = sbrk(4096);
  if(c != (char*)0xffffffff){
    fprintf(stdout, "sbrk allocated more than 632K, c %x\n", c);
    exit();
  }
#endif

  // can one de-allocate?
  a = sbrk(0);
  c = sbrk(-4096);
  if(c == (char*)0xffffffff){
    fprintf(stdout, "sbrk could not deallocate\n");
    exit();
  }
  c = sbrk(0);
  if(c != a - 4096){
    fprintf(stdout, "sbrk deallocation produced wrong address, a %x c %x\n", a, c);
    exit();
  }

  // can one re-allocate that page?
  a = sbrk(0);
  c = sbrk(4096);
  if(c != a || sbrk(0) != a + 4096){
    fprintf(stdout, "sbrk re-allocation failed, a %x c %x\n", a, c);
    exit();
  }
#if 0
  if(*lastaddr == 99){
    // should be zero
    fprintf(stdout, "sbrk de-allocation didn't really deallocate\n");
    exit();
  }
#endif

#if 0
  c = sbrk(4096);
  if(c != (char*)0xffffffff){
    fprintf(stdout, "sbrk was able to re-allocate beyond 632K, c %x\n", c);
    exit();
  }
#endif

#if 0
  // can we read the kernel's memory?
  for(a = (char*)(640*1024); a < (char*)2000000; a += 50000){
    ppid = getpid();
    pid = fork(0);
    if(pid < 0){
      fprintf(stdout, "fork failed\n");
      exit();
    }
    if(pid == 0){
      fprintf(stdout, "oops could read %x = %x\n", a, *a);
      kill(ppid);
      exit();
    }
    wait(-1);
  }
#endif

  // if we run the system out of memory, does it clean up the last
  // failed allocation?
  sbrk(-(sbrk(0) - oldbrk));
  if(pipe(fds, 0) != 0){
    fprintf(1, "pipe() failed\n");
    exit();
  }
  for(i = 0; i < sizeof(pids)/sizeof(pids[0]); i++){
    if((pids[i] = fork(0)) == 0){
      // allocate the full 632K
      sbrk((632 * 1024) - (uptr)sbrk(0));
      write(fds[1], "x", 1);
      // sit around until killed
      for(;;) nsleep(1000000000ull);
    }
    if(pids[i] != -1)
      read(fds[0], &scratch, 1);
  }
  // if those failed allocations freed up the pages they did allocate,
  // we'll be able to allocate here
  c = sbrk(4096);
  for(i = 0; i < sizeof(pids)/sizeof(pids[0]); i++){
    if(pids[i] == -1)
      continue;
    kill(pids[i]);
    wait(-1);
  }
  if(c == (char*)0xffffffff){
    fprintf(stdout, "failed sbrk leaked memory\n");
    exit();
  }

  if(sbrk(0) > oldbrk)
    sbrk(-(sbrk(0) - oldbrk));

  fprintf(stdout, "sbrk test OK\n");
}

void
validatetest(void)
{
  int pid;
  uptr lo, hi, p;

  fprintf(stdout, "validate test\n");
  // Do 16 pages below KBASE and 16 pages above,
  // which should be code pages and read-only
  lo = 0xFFFFFF0000000000ull - 16*4096;
  hi = 0xFFFFFF0000000000ull + 16*4096;

  for(p = lo; p <= hi; p += 4096){
    if((pid = fork(0)) == 0){
      // try to crash the kernel by passing in a badly placed integer
      if (pipe((int*)p, 0) == 0)
        fprintf(stdout, "validatetest failed (pipe succeeded)\n");
      exit();
    }
    nsleep(0);
    nsleep(0);
    kill(pid);
    wait(-1);

    // try to crash the kernel by passing in a bad string pointer
    if(link("nosuchfile", (char*)p) != -1){
      fprintf(stdout, "link should not succeed\n");
      exit();
    }
  }

  fprintf(stdout, "validate ok\n");
}

// does unintialized data start out zero?
char uninit[10000];
void
bsstest(void)
{
  int i;

  fprintf(stdout, "bss test\n");
  for(i = 0; i < sizeof(uninit); i++){
    if(uninit[i] != '\0'){
      fprintf(stdout, "bss test failed\n");
      exit();
    }
  }
  fprintf(stdout, "bss test ok\n");
}

// does exec do something sensible if the arguments
// are larger than a page?
void
bigargtest(void)
{
  int pid;

  pid = fork(0);
  if(pid == 0){
    const char *args[32+1];
    int i;
    for(i = 0; i < 32; i++)
      args[i] = "bigargs test: failed\n                                                                                                                     ";
    args[32] = 0;
    fprintf(stdout, "bigarg test\n");
    exec("echo", args);
    fprintf(stdout, "bigarg test ok\n");
    exit();
  } else if(pid < 0){
    fprintf(stdout, "bigargtest: fork failed\n");
    exit();
  }
  wait(-1);
}

void
uox(char *name, const char *data)
{
  int fd = open(name, O_CREAT|O_RDWR, 0666);
  if(fd < 0){
    fprintf(stdout, "creat %s failed\n", name);
    exit();
  }
  if(write(fd, "xx", 2) != 2){
    fprintf(stdout, "write failed\n");
    exit();
  }
  close(fd);
}

// test concurrent unlink / open.
void
unopentest(void)
{
  fprintf(stdout, "concurrent unlink/open\n");

  int pid = fork(0);
  if(pid == 0){
    while(1){
      for(int i = 0; i < 1; i++){
        char name[32];
        name[0] = 'f';
        name[1] = 'A' + i;
        name[2] = '\0';
        int fd = open(name, O_RDWR);
        if(fd >= 0)
          close(fd);
        fd = open(name, O_RDWR);
        if(fd >= 0){
          if(write(fd, "y", 1) != 1){
            fprintf(stdout, "write %s failed\n", name);
            exit();
          }
          close(fd);
        }
      }
    }
  }

  for(int iters = 0; iters < 1000; iters++){
    for(int i = 0; i < 1; i++){
      char name[32];
      name[0] = 'f';
      name[1] = 'A' + i;
      name[2] = '\0';
      uox(name, "xxx");
      if(unlink(name) < 0){
        fprintf(stdout, "unlink %s failed\n", name);
        exit();
      }
      // reallocate that inode
      name[0] = 'g';
      if(mkdir(name, 0777) != 0){
        fprintf(stdout, "mkdir %s failed\n", name);
        exit();
      }
    }
    for(int i = 0; i < 10; i++){
      char name[32];
      name[0] = 'f';
      name[1] = 'A' + i;
      name[2] = '\0';
      unlink(name);
      name[0] = 'g';
      unlink(name);
    }
  }
  kill(pid);
  wait(-1);

  fprintf(stdout, "concurrent unlink/open ok\n");
}

void
preads(void)
{
  static const int fsize = (64 << 10);
  static const int bsize = 4096;
  static const int nprocs = 4;
  static const int iters = 100;
  static char buf[bsize];
  int fd;
  int pid;

  fprintf(1, "concurrent preads\n");

  fd = open("preads.x", O_CREAT|O_RDWR, 0666);
  if (fd < 0)
    die("preads: open failed");

  for (int i = 0; i < fsize/bsize; i++)
    if (write(fd, buf, bsize) != bsize)
      die("preads: write failed");
  close(fd);

  for (int i = 0; i < nprocs; i++) {
    pid = fork(0);
    if (pid < 0)
      die("preads: fork failed");
    if (pid == 0)
      break;
  }

  for (int k = 0; k < iters; k++) {
    fd = open("preads.x", O_RDONLY);
    for (int i = 0; i < fsize; i+=bsize)
      if (pread(fd, buf, bsize, i) != bsize)
        die("preads: pread failed");
    close(fd);
  }

  if (pid == 0)
    exit();

  for (int i = 0; i < nprocs; i++)
    wait(-1);

  fprintf(1, "concurrent preads OK\n");
}

void
tls_test(void)
{
  printf("tls_test\n");
  u64 buf[128];

  for (int i = 0; i < sizeof(buf) / sizeof(buf[0]); i++)
    buf[i] = 0x11deadbeef2200 + i;

  for (int i = 0; i < sizeof(buf) / sizeof(buf[0]) - 1; i++) {
    setfs((uptr) &buf[i]);

    u64 x;
    u64 exp = 0x11deadbeef2200 + i;
    __asm volatile("movq %%fs:0, %0" : "=r" (x));
    if (x != buf[i] || x != exp)
      fprintf(2, "tls_test: 0x%lx != 0x%lx\n", x, buf[0]);

    getpid();  // make sure syscalls don't trash %fs
    __asm volatile("movq %%fs:0, %0" : "=r" (x));
    if (x != buf[i] || x != exp)
      fprintf(2, "tls_test: 0x%lx != 0x%lx again\n", x, buf[0]);

    __asm volatile("movq %%fs:8, %0" : "=r" (x));
    if (x != buf[i+1] || x != exp+1)
      fprintf(2, "tls_test: 0x%lx != 0x%lx next\n", x, buf[0]);
  }
  fprintf(1, "tls_test ok\n");
}

static pthread_barrier_t ftable_bar;
static volatile int ftable_fd;

static void*
ftablethr(void *arg)
{
  char buf[32];
  int r;

  pthread_barrier_wait(&ftable_bar);
  
  r = read(ftable_fd, buf, sizeof(buf));
  if (r < 0)
    fprintf(2, "ftablethr: FAILED bad fd\n");
  return 0;
}

static void
ftabletest(void)
{
  printf("ftabletest...\n");
  pthread_barrier_init(&ftable_bar, 0, 2);

  pthread_t th;
  pthread_create(&th, 0, &ftablethr, 0);

  ftable_fd = open("README", 0);
  if (ftable_fd < 0)
    die("open");

  pthread_barrier_wait(&ftable_bar);
  wait(-1);
  printf("ftabletest ok\n");
}

static pthread_key_t tkey;
static pthread_barrier_t bar0, bar1;
enum { nthread = 8 };

static void*
thr(void *arg)
{
  pthread_setspecific(tkey, arg);
  pthread_barrier_wait(&bar0);

  u64 x = (u64) arg;
  if ((x >> 8) != 0xc0ffee)
    fprintf(2, "thr: x 0x%lx\n", x);
  if (arg != pthread_getspecific(tkey))
    fprintf(2, "thr: arg %p getspec %p\n", arg, pthread_getspecific(tkey));

  pthread_barrier_wait(&bar1);
  return 0;
}

void
thrtest(void)
{
  printf("thrtest\n");

  pthread_key_create(&tkey, 0);
  pthread_barrier_init(&bar0, 0, nthread);
  pthread_barrier_init(&bar1, 0, nthread+1);

  for(int i = 0; i < nthread; i++) {
    pthread_t tid;
    pthread_create(&tid, 0, &thr, (void*) (0xc0ffee00ULL | i));
  }

  pthread_barrier_wait(&bar1);

  for(int i = 0; i < nthread; i++)
    wait(-1);

  printf("thrtest ok\n");
}

void
unmappedtest(void)
{
  // Chosen to conflict with default start addr in kernel
  off_t off = 0x1000;

  printf("unmappedtest\n");
  for (int i = 1; i <= 8; i++) {
    void *p = mmap((void*)off, i*4096, PROT_READ|PROT_WRITE,
                   MAP_PRIVATE|MAP_FIXED|MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
      die("unmappedtest: map failed");
    off += (i*2*4096);
  }

  for (int i = 8; i >= 1; i--) {
    void *p = mmap(0, i*4096, PROT_READ|PROT_WRITE,
                   MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
      die("unmappedtest: map failed");
    int r = munmap(p, i*4096);
    if (r < 0)
      die("unmappedtest: unmap failed");
  }
  
  off = 0x1000;
  for (int i = 1; i <= 8; i++) {
    int r = munmap((void*)off, i*4096);
    if (r < 0)
      die("unmappedtest: unmap failed");
    off += (i*2*4096);
  }
  printf("unmappedtest ok\n");
}

bool
test_fault(char *p)
{
  int fds[2], pid;
  char buf = 0;

  if (pipe(fds, 0) != 0)
    die("test_fault: pipe failed");
  if ((pid = fork(0)) < 0)
    die("test_fault: fork failed");

  if (pid == 0) {
    close(fds[0]);
    *p = 0x42;
    if (write(fds[1], &buf, 1) != 1)
      die("test_fault: write failed");
    exit();
  }

  close(fds[1]);
  bool faulted = (read(fds[0], &buf, 1) < 1);
  wait(-1);
  close(fds[0]);
  return faulted;
}

void
vmoverlap(void)
{
  printf("vmoverlap\n");

  char *base = (char*)0x1000;
  char map[10] = {};
  int mapn = 1;
  for (int i = 0; i < 100; i++) {
    int op = i % 20 >= 10;
    int lo = rnd() % 10, hi = rnd() % 10;
    if (lo > hi)
      std::swap(lo, hi);
    if (lo == hi)
      continue;

    if (op == 0) {
      // Map
      void *res = mmap(base + lo * 4096, (hi-lo) * 4096, PROT_READ|PROT_WRITE,
                       MAP_PRIVATE|MAP_FIXED|MAP_ANONYMOUS, -1, 0);
      if (res == MAP_FAILED)
        die("vmoverlap: mmap failed");
    } else {
      // Unmap
      int res = munmap(base + lo * 4096, (hi-lo) * 4096);
      if (res < 0)
        die("vmoverlap: munmap failed");
    }

    for (int i = lo; i < hi; i++) {
      if (op == 0) {
        // Check that it zeroed the range
        if (base[i*4096] != 0)
          die("did not zero mapped-over region at %p", &base[i*4096]);
        // Fill it in
        base[i*4096] = mapn;
        // Update the expected mapping
        map[i] = mapn;
      } else {
        // Update the expected mapping
        map[i] = 0;
      }
    }

    // Check entire mapping
    for (int i = 0; i < sizeof(map)/sizeof(map[0]); i++) {
      if (map[i] && base[i*4096] != map[i])
        die("page outside of mapped-over region changed");
      else if (!map[i] && !test_fault(&base[i*4096]))
        die("expected fault");
    }
  }

  munmap(base, 10 * 4096);

  printf("vmoverlap ok\n");
}

void*
vmconcurrent_thr(void *arg)
{
  int core = (uintptr_t)arg;
  setaffinity(core);

  char *base = (char*)0x1000;
  for (int i = 0; i < 500; ++i) {
    void *res = mmap(base, 4096, PROT_READ|PROT_WRITE,
                     MAP_PRIVATE|MAP_FIXED|MAP_ANONYMOUS, -1, 0);
    if (res == MAP_FAILED)
      die("vmconcurrent_thr: mmap failed");
    *(char*)res = 42;
  }
  return nullptr;
}

void
vmconcurrent(void)
{
  printf("vmconcurrent\n");

  for (int i = 0; i < nthread; i++) {
    pthread_t tid;
    pthread_create(&tid, 0, &vmconcurrent_thr, (void*)(uintptr_t)i);
  }

  for(int i = 0; i < nthread; i++)
    wait(-1);

  printf("vmconcurrent ok\n");
}

void*
tlb_thr(void *arg)
{
  // Pass a token around and have each thread read from the mapped
  // region, re-map the region, then write to the mapped region.
  static volatile int curcore = 0;
  int core = (uintptr_t)arg;
  setaffinity(core);

  volatile char *base = (char*)0x1000;
  for (int i = 0; i < 50; i++) {
    while (core != curcore);
    if (core > 0)
      assert(*base == i + core - 1);
    else if (i != 0)
      assert(*base == i + nthread - 2);
    void *res = mmap((void*)base, 4096, PROT_READ|PROT_WRITE,
                     MAP_PRIVATE|MAP_FIXED|MAP_ANONYMOUS, -1, 0);
    if (res == MAP_FAILED)
      die("tlb_thr: mmap failed");
    *base = i + core;
    curcore = (curcore + 1) % nthread;
  }
  return nullptr;
}

void
tlb(void)
{
  printf("tlb\n");

  for (int i = 0; i < nthread; i++) {
    pthread_t tid;
    pthread_create(&tid, 0, &tlb_thr, (void*)(uintptr_t)i);
  }

  for(int i = 0; i < nthread; i++)
    wait(-1);

  printf("tlb ok\n");
}

void*
float_thr(void *arg)
{
  setaffinity(0);

  for (int i = 0; i < 100; ++i) {
    double x = 1;
    int y = 1;

    for (int j = 0; j < 20; ++j) {
      x *= 2;
      y *= 2;
      yield();
    }

    assert(x == y);
  }
  ++(*(std::atomic<int>*)arg);
  return nullptr;
}

void
floattest(void)
{
  printf("floattest\n");

  std::atomic<int> success(0);
  for (int i = 0; i < nthread; i++) {
    pthread_t tid;
    pthread_create(&tid, 0, &float_thr, (void*)&success);
  }

  for(int i = 0; i < nthread; i++)
    wait(-1);

  if (success != nthread)
    die("not all float_thrs succeeded");

  printf("floattest ok\n");
}

static int nenabled;
static char **enabled;

void
run_test(const char *name, void (*test)())
{
  if (!nenabled) {
    test();
  } else {
    for (int i = 0; i < nenabled; i++) {
      if (strcmp(name, enabled[i]) == 0) {
        test();
        break;
      }
    }
  }
}

int
main(int argc, char *argv[])
{
  fprintf(1, "usertests starting\n");

  if(open("usertests.ran", 0) >= 0){
    fprintf(1, "already ran user tests -- rebuild fs.img\n");
    exit();
  }
  close(open("usertests.ran", O_CREAT, 0666));

  nenabled = argc - 1;
  enabled = argv + 1;

#define TEST(name) run_test(#name, name)

  TEST(memtest);
  TEST(unopentest);
  TEST(bigargtest);
  TEST(bsstest);
  TEST(sbrktest);

  // we should be able to grow a user process to consume all phys mem

  TEST(unmappedtest);
  TEST(vmoverlap);
  TEST(vmconcurrent);
  TEST(tlb);

  TEST(validatetest);

  TEST(opentest);
  TEST(writetest);
  TEST(writetest1);
  TEST(createtest);
  TEST(preads);

  // TEST(mem);
  TEST(pipe1);
  TEST(preempt);
  TEST(exitwait);

  TEST(rmdot);
  TEST(thirteen);
  TEST(longname);
  TEST(bigfile);
  TEST(subdir);
  TEST(concreate);
  TEST(linktest);
  TEST(unlinkread);
  TEST(createdelete);
  TEST(twofiles);
  TEST(sharedfd);
  TEST(dirfile);
  TEST(iref);
  TEST(forktest);
  TEST(bigdir); // slow
  TEST(tls_test);
  TEST(thrtest);
  TEST(ftabletest);

  TEST(floattest);

  TEST(exectest);               // Must be last

  exit();
}
