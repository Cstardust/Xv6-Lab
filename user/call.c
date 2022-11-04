#include "kernel/param.h"
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int g(int x) {
  return x+3;
}

int f(int x) {
  return g(x);
}

void main(void) {
  printf("%d %d\n", f(8)+1, 13);  //  a1  a2
  

  unsigned int i = 0x00646c72;
	printf("H%x Wo%s\n", 57616, &i); 

  unsigned int j = 0x726c6400;
	printf("H%x Wo%s\n", 57616, &j); 

  printf("x=%d y=%d\n", 3);
  exit(0);
}
