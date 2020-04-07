/*
 * Port utility from s2ecmd and s2eget into s2e.so
 */

#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <s2e/seed_searcher.h>
#include <s2e/s2e.h>

#include "s2e_so.h"

typedef unsigned char u8;
typedef int s32;
typedef unsigned int u32;

#define BUFFER_SIZE 1024 * 64

#define alloc_printf(_str...) ({ \
    char* _tmp; \
    s32 _len = snprintf(NULL, 0, _str); \
    if (_len < 0) printf("Whoa, snprintf() fails?!"); \
    _tmp = malloc(_len + 1); \
    snprintf((char*)_tmp, _len + 1, _str); \
    _tmp; \
  })


static int copy_file(const char *dest_file, const char *host_file) {
    const char *path = NULL;
    int retval = -1;
    int fd = -1;
    int s2e_fd = -1;

    path = dest_file;
    if (!path) {
        goto end;
    }

    // Delete anything that already exists at this location
    unlink(path);

    // Open the host file path for reading.
    s2e_fd = s2e_open(host_file);
    if (s2e_fd < 0) {
        fprintf(stderr, "s2e_open of %s failed\n", host_file);
        goto end;
    }

// Open the destination file path for writing
#ifdef _WIN32
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, S_IRWXU);
#else
    fd = creat(path, S_IRWXU);
#endif
    if (fd < 0) {
        fprintf(stderr, "Could not create file %s\n", path);
        goto end;
    }

    int fsize = 0;
    char buf[BUFFER_SIZE] = {0};

    // Copy and write data from host to guest
    while (1) {
        int ret = s2e_read(s2e_fd, buf, sizeof(buf));
        if (ret == -1) {
            fprintf(stderr, "s2e_read failed\n");
            goto end;
        } else if (ret == 0) {
            break;
        }

        int ret1 = write(fd, buf, ret);
        if (ret1 != ret) {
            fprintf(stderr, "Could not write to file\n");
            goto end;
        }

        fsize += ret;
    }

    printf("... file %s of size %d was transferred successfully to %s\n", host_file, fsize, path);

    retval = 0;

end:
    if (s2e_fd >= 0) {
        s2e_close(s2e_fd);
    }

    if (fd >= 0) {
        close(fd);
    }

    if (retval < 0) {
        // There was an error, clean up any partially transferred files
        if (path) {
            unlink(path);
        }
        retval = -retval;
    }

    return retval;
}

///
/// \brief Encode a name, chunk id, and total chunks into a symbolic variable name
///
/// This variable name will be used by the TestCaseGenerator plugin in order
/// to reconstruct the concrete input files.
///
/// \param cleaned_name the original file path stripped of any special characters
/// \param current_chunk the chunk identifier
/// \param total_chunks how many chunks are expected for the file
/// \return the variable name
///
static char* get_chunk_name(const char *cleaned_name, unsigned current_chunk, unsigned total_chunks) {
    char *name = alloc_printf("__symfile___%s__%d_%d_symfile__", cleaned_name, current_chunk, total_chunks);
    return name;
}

///
/// \brief Make the specified file chunk symbolic
///
/// \param fd the descriptor of the file to be made symbolic (must be located in a ram disk)
/// \param offset the offset in the file to be made symbolic
/// \param buffer the pointer where to store the original concrete data
/// \param buffer_size the size of the buffer in bytes
/// \param variable_name the name of the variable that encodes the chunk information
/// \return the number of bytes read/written to the file
///
static ssize_t make_chunk_symbolic(int fd, off_t offset, void *buffer, unsigned buffer_size,
                                   const char* variable_name) {
    // Read the file in chunks and make them symbolic
    if (lseek(fd, offset, SEEK_SET) < 0) {
        s2e_kill_state_printf(-1, "symbfile: could not seek to position %d", offset);
        return -3;
    }

    // Read the data
    ssize_t read_count = read(fd, buffer, buffer_size);
    if (read_count < 0) {
        s2e_kill_state_printf(-1, "symbfile: could not read from file");
        return -4;
    }

    // Make the buffer symbolic
    s2e_make_symbolic(buffer, read_count, variable_name);

    // Write it back
    if (lseek(fd, offset, SEEK_SET) < 0) {
        s2e_kill_state_printf(-1, "symbfile: could not seek to position %d", offset);
        return -5;
    }

    ssize_t written_count = write(fd, buffer, read_count);
    if (written_count < 0) {
        s2e_kill_state_printf(-1, "symbfile: could not write to file");
        return -6;
    }

    if (read_count != written_count) {
        // XXX: should probably retry...
        s2e_kill_state_printf(-1, "symbfile: could not write the read amount");
        return -7;
    }

    return read_count;
}

///
/// \brief Make the entire file symbolic
///
/// \param fd the descriptor of the file to be made symbolic (must be on a ram disk)
/// \param file_size the size in bytes of the file
/// \param block_size the size of a chunk (or symbolic variable)
/// \param cleaned_name the sanitized name of the file
/// \return error code (0 on success)
///
static int make_whole_file_symbolic(int fd, unsigned file_size, unsigned block_size, const char *cleaned_name) {
    char buffer[block_size];

    unsigned current_chunk = 0;
    unsigned total_chunks = file_size / sizeof(buffer);
    if (file_size % sizeof(buffer)) {
        ++total_chunks;
    }

    off_t offset = 0;
    do {
        ssize_t totransfer = file_size > sizeof(buffer) ? sizeof(buffer) : file_size;

        char* name = get_chunk_name(cleaned_name, current_chunk, total_chunks);
        ssize_t read_count = make_chunk_symbolic(fd, offset, buffer, totransfer, name);
        free(name);

        offset += read_count;
        file_size -= read_count;
        ++current_chunk;
    } while (file_size > 0);

    return 0;
}

int handler_symbfile(const char *filename) {
    int ret = 0;
    int flags = O_RDWR;

#ifdef _WIN32
    flags |= O_BINARY;
#endif
    unsigned block_size = 0x1000;
    int fd = open(filename, flags);
    if (fd < 0) {
        s2e_kill_state_printf(-1, "symbfile: could not open %s\n", filename);
        return -1;
    }

    // Determine the size of the file
    off_t size = lseek(fd, 0, SEEK_END);
    if (size < 0) {
        s2e_kill_state_printf(-1, "symbfile: could not determine the size of %s\n", filename);
        return -2;
    }

    ret = make_whole_file_symbolic(fd, size, block_size, filename);
    close(fd);
    return ret;
}

void initialize_seed() {
    if (!getenv("FORK_SERVER")) {
        return;
    }

    // 1. Get the seed path
    char seed_file[256] = {0};
    int should_fork = 0;
    s2e_seed_get_file(seed_file, sizeof(seed_file), &should_fork);

    // 2. Move the seed from host to guest
    copy_file("/tmp/input", seed_file);

    // 3. Make the seed symbolic
    handler_symbfile("/tmp/input");
}

static int handler_seedsearcher_enable() __attribute__((unused));
static int handler_seedsearcher_enable() {
    s2e_seed_searcher_enable();
    return 0;
}
