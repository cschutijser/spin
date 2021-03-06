#ifndef SPIN_MAINLOOP_H
#define SPIN_MAINLOOP_H

typedef void (*workfunc)(void*, int, int);

void mainloop_register(char *name, workfunc wf, void *arg, int fd, int toval);
void mainloop_run();
void mainloop_end();
#endif
