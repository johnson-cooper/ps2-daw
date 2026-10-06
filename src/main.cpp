// PS2 DAW entry point. All real work lives in App (src/app.cpp).
#include "app.hpp"

// Static storage: the App aggregates the engine, mixer buffers, command
// queue and project (~40 KiB); keeping it off the main thread stack.
static App g_app;

int main(int argc, char** argv)
{
    return g_app.run(argc, argv);
}
