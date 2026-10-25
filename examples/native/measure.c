#define _GNU_SOURCE
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
/* Benchmark-only parent: avoid inheriting a large Python process RSS floor. */
int main(int argc, char **argv) {
  if (argc < 5)
    return 2;
  struct timespec start, end;
  struct rusage usage;
  int status;
  clock_gettime(CLOCK_MONOTONIC, &start);
  pid_t pid = fork();
  if (pid < 0)
    return 3;
  if (pid == 0) {
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (getppid() == 1)
      _exit(125);
    int out = open(argv[1], O_WRONLY | O_CREAT | O_TRUNC, 0600),
        err = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (out < 0 || err < 0 || dup2(out, STDOUT_FILENO) < 0 ||
        dup2(err, STDERR_FILENO) < 0)
      _exit(126);
    close(out);
    close(err);
    execv(argv[4], argv + 4);
    _exit(127);
  }
  if (wait4(pid, &status, 0, &usage) < 0)
    return 4;
  clock_gettime(CLOCK_MONOTONIC, &end);
  double wall = (double)(end.tv_sec - start.tv_sec) +
                (double)(end.tv_nsec - start.tv_nsec) / 1e9;
  double cpu = (double)usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1e6 +
               (double)usage.ru_stime.tv_sec + usage.ru_stime.tv_usec / 1e6;
  FILE *file = fopen(argv[3], "w");
  if (!file)
    return 5;
  fprintf(file,
          "{\"status\":%d,\"wall_seconds\":%.9f,\"cpu_seconds\":%.9f,\"peak_"
          "rss_bytes\":%ld}",
          WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status),
          wall, cpu, usage.ru_maxrss * 1024L);
  return fclose(file) != 0;
}
