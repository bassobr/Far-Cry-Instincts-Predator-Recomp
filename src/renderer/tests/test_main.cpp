// Shared test run (King Kong's check.h): predator_renderer_tests and renderer_disc_tests.
//   <exe> [filter]   only cases whose name contains `filter`
#include "kk/backend/tests/check.h"

int main(int argc, char** argv) { return fcp::nr::test::RunAll(argc, argv); }
