#include "include.h"

kernel void test_kernel(global int* out)
{
  out[0] = test_function();
}

