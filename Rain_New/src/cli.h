#ifndef RAIN_CLI_H
#define RAIN_CLI_H
/* Command line front end shared by the console build and by the GUI exe (for diagnostics like `rain.exe probe`). */
int cli_run(int argc, char **argv);
#endif
