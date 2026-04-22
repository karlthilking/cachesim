#include <iostream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <errno.h>
#include <err.h>
#include <sys/wait.h>

bool is_c_source(std::string_view source)
{
    return source.ends_with(".c");
}

bool is_cpp_source(std::string_view source)
{
    return (source.ends_with(".cpp") || source.ends_with(".cxx") ||
            source.ends_with(".cc") || source.ends_with(".c++"));
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        std::cerr << "Usage: ./cachesim_launch <source-file>\n";
        return -1;
    }
    
    std::string source(argv[1]);
    std::string binary;
    std::string compiler;
    
    if (is_cpp_source(source)) {
        compiler = "clang++";
        binary = source.substr(0, source.size() - 4);
        if (auto pos = binary.rfind('/'); pos != std::string::npos)
            binary = "./" + binary.substr(pos + 1);
    } else if (is_c_source(source)) {
        compiler = "clang";
        binary = source.substr(0, source.size() - 2);
        if (auto pos = binary.rfind('/'); pos != std::string::npos)
            binary = "./" + binary.substr(pos + 1);
    } else {
        std::cerr << "Unsupported source type: " << source << '\n';
        return -1;
    }
    
    const char *args[] = {
        compiler.c_str(),
        "-L.", "-lcache", "-Wl,-rpath,.",
        "-fpass-plugin=./libcachesimpass.so",
        "-o",
        binary.c_str(),
        source.c_str(),
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
        binary.c_str(),
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
    
    unlink(binary.c_str());
    if (WIFEXITED(wstat) && WEXITSTATUS(wstat) != 0)
        return -1;

    return 0;
}
