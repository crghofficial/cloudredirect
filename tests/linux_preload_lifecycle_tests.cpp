#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc != 2 || !dlsym(RTLD_DEFAULT, "CR_GetVersion"))
        return 1;
    // Let the preload start waiting for steamclient. A short-lived Steam
    // launcher must retain LD_PRELOAD for the runtime child and exit promptly.
    usleep(200000);
    const char* preload = std::getenv("LD_PRELOAD");
    if (!preload || std::strcmp(preload, argv[1]) != 0)
        return 2;
    return 0;
}
