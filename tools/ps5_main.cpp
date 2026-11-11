#include "../host/cli_entry_points.h"
#include "../core/remote/client.h"
int CortexPs5Main(int argc, char** argv) { return cortex::remote::RemoteMain(argc, argv); }
