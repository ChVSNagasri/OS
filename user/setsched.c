#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  int policy;

  if (argc != 2) {
    printf("Usage: setsched rr|fcfs|mlfq\n");
    exit(1);
  }

  if (strcmp(argv[1], "rr") == 0) {
    policy = 0;
  } else if (strcmp(argv[1], "fcfs") == 0) {
    policy = 1;
  } else if (strcmp(argv[1], "mlfq") == 0) {
    policy = 2;
  } else {
    printf("Invalid policy. Use rr, fcfs, or mlfq.\n");
    exit(1);
  }

  if (setsched(policy) < 0) {
    printf("Failed to change scheduler.\n");
    exit(1);
  }

  printf("Scheduler changed successfully.\n");
  exit(0);
}
