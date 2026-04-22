#include <iostream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <errno.h>
#include <err.h>
#include <sys/wait.h>

void remove_extension(char *filename)
{
    for (size_t i = strlen(filename) - 1; i >= 0; i--) {
        if (filename[i] == '.') {
            filename[i] = '\0';
            return;
        }
    }
}

char *strip_path(char *filepath)
{
    for (char *s = filepath + strlen(filepath) - 1; s >= filepath; s--) {
        if (*s == '/')
            return s + 1;
    }
    return filepath;
}

bool is_c_source(char *source)
{
    char *s = source + strlen(source) - 2;
    if (strncmp(s, ".c", 2) == 0)
        return true;
    return false;
}

bool is_cpp_source(char *source)
{
    char *s = source + strlen(source) - 4;
    if (strncmp(s, ".cpp", 4) == 0)
        return true;
    return false;
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        std::cerr << "Usage: ./cachesim_launch <source-file>\n";
        return -1;
    }

    char source[128], binary[128], compiler[32];
    
    strncpy(source, argv[1], strlen(argv[1]) + 1);
    strncpy(binary, "./", 3);
    strncat(binary, strip_path(source), strlen(strip_path(source)));
    remove_extension(binary);
    
    if (is_cpp_source(source))
        strncpy(compiler, "clang++", strlen("clang++") + 1);
    else if (is_c_source(source))
        strncpy(compiler, "clang", strlen("clang") + 1);
    else {
        std::cerr << "Unrecognized source type: " << source << '\n';
        exit(-1);
    }
    
    const char *args[] = {
        const_cast<const char*>(compiler),
        "-L.", "-lcache", "-Wl,-rpath,.",
        "-fpass-plugin=./libcachesimpass.so",
        "-o",
        const_cast<const char *>(binary),
        const_cast<const char *>(source),
        nullptr
    };
    
    int wstat;
    switch (fork()) {
    case -1:
        err(EXIT_FAILURE, "fork");
    case 0:
        execvp(args[0], const_cast<char **>(args));
        exit(-1);
    default:
        waitpid(-1, &wstat, 0);
        if (WIFEXITED(wstat) && WEXITSTATUS(wstat) != 0) {
            std::cerr << "Failed to compile " << source << '\n';
            exit(-1);
        }
    }
    
    const char *exec_args[] = {
        binary,
        nullptr
    };

    switch (fork()) {
    case -1:
        err(EXIT_FAILURE, "fork");
    case 0:
        std::cout << "Executing " << binary << '\n';
        if (execvp(exec_args[0], const_cast<char **>(exec_args)) < 0) {
            std::cerr << "Failed to execute: " << binary
                      << ": " << strerror(errno) << '\n';
            exit(-1);
        }
    default:
        waitpid(-1, &wstat, 0);
    }
    
    unlink(binary);
    if (WIFEXITED(wstat) && WEXITSTATUS(wstat) != 0)
        return -1;

    return 0;
}
