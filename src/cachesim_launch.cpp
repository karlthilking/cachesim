#include <iostream>
#include <type_traits>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <cstdio>
#include <errno.h>
#include <err.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sys/sysinfo.h>

bool is_param(char *option)
{
    return !(strncmp(option, "--L1d", 5) || strncmp(option, "--L1i", 5) ||
             strncmp(option, "--L2", 4)  || strncmp(option, "--L3", 4));
}

void get_params(char *option, size_t &size, 
                size_t &block_size, unsigned &assoc)
{
    char *s = strchr(option, '=') + 1;
    size = strtoul(s, nullptr, 10);

    s = strchr(s, ',') + 1;
    assoc = strtoul(s, nullptr, 10);

    s = strchr(s, ',') + 1;
    block_size = strtoul(s, nullptr, 10);
}

void setup_params(char **params, size_t n)
{
    FILE *f;
    if ((f = fopen("include/params.hpp", "r+")) == nullptr)
        err(EXIT_FAILURE, "fopen");

    fprintf(f, "#ifndef __CACHE_PARAMS_HPP__\n"
               "#define __CACHE_PARAMS_HPP__\n"
               "namespace cachesim {\n"
               "\tconstexpr size_t ncpus = %zu;\n", 
               static_cast<size_t>(get_nprocs()));
    
    size_t      l1d_size = 1 << 15,         l1i_size = 1 << 15,
                l2_size = 1 << 20,          l3_size = 1 << 24,
                l1d_block_size = 1 << 6,    l1i_block_size = 1 << 6,
                l2_block_size = 1 << 6,     l3_block_size = 1 << 6;

    unsigned    l1d_assoc = 8u,             l1i_assoc = 8u, 
                l2_assoc = 16u,             l3_assoc = 16u;
            
    for (auto i = 0u; i < n; i++) {
        if (!strncmp(params[i], "--L1d", 5))
            get_params(params[i], l1d_size, l1d_block_size, l1d_assoc);
        else if (!strncmp(params[i], "--L1i", 5))
            get_params(params[i], l1i_size, l1i_block_size, l1i_assoc);
        else if (!strncmp(params[i], "--L2", 4))
            get_params(params[i], l2_size, l2_block_size, l2_assoc);
        else if (!strncmp(params[i], "--L3", 4))
            get_params(params[i], l3_size, l3_block_size, l3_assoc);
    }

    fprintf(f, "constexpr size_t    l1d_size        = %zu\n"
               "constexpr size_t    l1d_block_size  = %zu\n" 
               "constexpr unsigned  l1d_assoc       = %u\n"
               "constexpr size_t    l1i_size        = %zu\n"
               "constexpr size_t    l1i_block_size  = %zu\n"
               "constexpr unsigned  l1i_assoc       = %u\n"
               "constexpr size_t    l2_size         = %zu\n"
               "constexpr size_t    l2_block_size   = %zu\n"
               "constexpr unsigned  l2_assoc        = %u\n"
               "constexpr size_t    l3_size         = %zu\n"
               "constexpr size_t    l3_block_size   = %zu\n"
               "constexpr unsigned  l3_assoc        = %u\n",
               l1d_size, l1d_block_size, l1d_assoc,
               l1i_size, l1i_block_size, l1i_assoc,
               l2_size, l2_block_size, l2_assoc,
               l3_size, l3_block_size, l3_assoc);

    fprintf(f, "} // namespace cachesim\n"
               "#endif // __CACHE_PARAMS_HPP__");

    fclose(f);
}

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
        std::cerr << "Usage: ./cachesim_launch "
                     "[compile flags] [cache options] [source file]\n";
        std::cerr << "Cache options:\n"
                  << "\t--L1d=[size],[assoc],[block-size]\n"
                  << "\t--L1i=[size],[assoc],[block-size]\n"
                  << "\t--L2=[size],[assoc],[block-size]\n"
                  << "\t--L3=[size],[assoc],[block-size]\n";
        return -1;
    }

    char source[128], binary[128], compiler[32];
    
    strncpy(source, argv[argc - 1], strlen(argv[argc - 1]) + 1);
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

    const char *args[16] = {
        const_cast<const char *>(compiler),
        "-L.", "-lcache", "-Wl,-rpath,.",
        "-fpass-plugin=./libcachesimpass.so"
    };
    
    unsigned int i;
    for (i = 1u; i < std::make_unsigned_t<int>(argc - 1); i++) {
        if (is_param(argv[i]))
            break;
        args[5 + i - 1] = argv[i];
    }

    args[5 + i - 1] = "-o";
    args[5 + i - 1 + 1] = const_cast<const char *>(binary);
    args[5 + i - 1 + 2] = const_cast<const char *>(source);
    args[5 + i - 1 + 3] = nullptr;
    
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
            for (auto i = 0u; args[i] != nullptr; i++)
                std::cerr << args[i] << ' ';
            std::cerr << '\n';
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
