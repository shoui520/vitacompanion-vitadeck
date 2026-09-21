#include <psp2/kernel/threadmgr.h>

int main(void)
{
  sceKernelDelayThread(5 * 1000 * 1000);
  return 0;
}
