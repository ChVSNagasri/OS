#ifndef SCHEDFORGE_H
#define SCHEDFORGE_H

// =========================================================
// SCHEDFORGE SCHEDULER POLICIES
// =========================================================

#define SCHED_RR      0
#define SCHED_FCFS    1
#define SCHED_MLFQ    2

// Default scheduler
#define DEFAULT_SCHEDULER SCHED_RR

// =========================================================
// ROUND ROBIN CONFIGURATION
// =========================================================

#define RR_QUANTUM 5

// =========================================================
// MLFQ CONFIGURATION
// =========================================================

#define MLFQ_LEVELS 3

#define MLFQ_Q0 2
#define MLFQ_Q1 4
#define MLFQ_Q2 8

// =========================================================
// SCHEDFORGE GLOBAL POLICY
// =========================================================

extern int schedforge_policy;

#endif
