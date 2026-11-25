// A module importing something the badge doesn't have: it must not load,
// and the error names the import.
#include "../tlib.h"

__attribute__((import_module("env"), import_name("launch_rockets"))) int launch_rockets(int n);

SOS_EXPORT(on_start) void on_start(void)
{
    say_int("rockets ", launch_rockets(3));
}
