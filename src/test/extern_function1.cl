extern int test_function();

kernel void test_kernel(global int* out)
{
  out[0] = test_function();
}

