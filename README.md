js-shell
=======

This is my implementation of the Spidermokey embedding
tutorial for Spidermonkey 24.

It is able to start several threads with their own runtimes.

The programm run can be used to execute simple js-scripts.
The programm repeat can be used to multipy the command line
input.

i.e.
./run $(./repeat 10 test.js)

will start 10 threads, each running test.js.

Insure, that CMake can find the pkg-conf file (set PKG_CONFIG_PATH).
