#include <iostream>

#include "../coroutine/coroutine.h"
#include "../coroutine/time.hpp"

using namespace CO;
using namespace std;

void* add(int a, int b) {
  cout << "calc ..." << a + b << endl;
  return reinterpret_cast<void*>(a + b);
}
void test(){};

int main() {
  Coroutine* c = Coroutine::getInstance();
  int i;
  for (i = 0; i < 10000; i++) {
    printf("***%d, hello, co_world!\n", i);
    c->co_create(bind(add, 1, 2), 1, 10240);
    Time::sleep_for(1);
  }
  return 0;
}
