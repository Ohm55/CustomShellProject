#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include "crypto.h"

#define MAGIC "FCRYPT1"
#define MAGIC_SIZE 7
#define VERSION 1
#define SALT_SIZE 16
#define NONCE_SIZE 12
#define TAG_SIZE 16
#define KEY_SIZE 32
/* Header fields are serialized in this order: magic, version, iterations, salt, nonce. */
#define HEADER_SIZE (MAGIC_SIZE + 1 + 4 + SALT_SIZE + NONCE_SIZE)
#define BUFFER_SIZE 65536
#define PBKDF2_ITERATIONS 600000U

/* Keep output private until a complete, successful operation can install it. */
struct output_file {
    int fd;
    char *temporary_path;
    const char *final_path;
    int force;
};

static void abandon_output(struct output_file *output);

static int write_all(int fd, const void *data, size_t length,
                     const volatile sig_atomic_t *cancelled)
{
    const unsigned char *position = data;
    ssize_t written;

    while (length > 0) {
        if (*cancelled) {
            errno = EINTR;
            return -1;
        }
        written = write(fd, position, length);
        if (written > 0) {
            position += written;
            length -= (size_t)written;
        } else if (written == -1 && errno == EINTR) {
            continue;
        } else {
            return -1;
        }
    }
    return 0;
}

static ssize_t read_retry(int fd, void *buffer, size_t length,
                          const volatile sig_atomic_t *cancelled)
{
    ssize_t count;
    do {
        if (*cancelled) {
            errno = EINTR;
            return -1;
        }
        count = read(fd, buffer, length);
    } while (count == -1 && errno == EINTR);
    return count;
}

static int read_exact(int fd, void *data, size_t length,
                      const volatile sig_atomic_t *cancelled)
{
    unsigned char *position = data;
    ssize_t count;

    while (length > 0) {
        count = read_retry(fd, position, length, cancelled);
        if (count <= 0)
            return -1;
        position += count;
        length -= (size_t)count;
    }
    return 0;
}

static void report_openssl_error(const char *message)
{
    fprintf(stderr, "filecrypt: %s\n", message);
    ERR_print_errors_fp(stderr);
}

static int derive_key(const unsigned char *password,
                      const unsigned char salt[SALT_SIZE],
                      uint32_t iterations,
                      unsigned char key[KEY_SIZE])
{
    size_t password_length = strlen((const char *)password);

    if (password_length > INT32_MAX || iterations > INT32_MAX)
        return -1;
    if (PKCS5_PBKDF2_HMAC((const char *)password, (int)password_length,
                          salt, SALT_SIZE, (int)iterations,
                          EVP_sha256(), KEY_SIZE, key) != 1) {
        report_openssl_error("key derivation failed");
        return -1;
    }
    return 0;
}

static int open_input(const char *input_path, const char *output_path,
                      int force, int *input_fd)
{
    struct stat input_stat;
    struct stat output_stat;

    *input_fd = open(input_path, O_RDONLY | O_CLOEXEC);
    if (*input_fd == -1) {
        fprintf(stderr, "filecrypt: cannot open input '%s': %s\n",
                input_path, strerror(errno));
        return -1;
    }
    if (fstat(*input_fd, &input_stat) == -1) {
        fprintf(stderr, "filecrypt: cannot inspect input '%s': %s\n",
                input_path, strerror(errno));
        close(*input_fd);
        return -1;
    }
    if (!S_ISREG(input_stat.st_mode)) {
        fprintf(stderr, "filecrypt: input must be a regular file\n");
        close(*input_fd);
        return -1;
    }

    if (stat(output_path, &output_stat) == 0) {
        if (input_stat.st_dev == output_stat.st_dev &&
            input_stat.st_ino == output_stat.st_ino) {
            fprintf(stderr, "filecrypt: input and output refer to the same file\n");
            close(*input_fd);
            return -1;
        }
        if (!force) {
            fprintf(stderr,
                    "filecrypt: output '%s' already exists (use -f to replace it)\n",
                    output_path);
            close(*input_fd);
            return -1;
        }
    } else if (errno != ENOENT) {
        fprintf(stderr, "filecrypt: cannot inspect output '%s': %s\n",
                output_path, strerror(errno));
        close(*input_fd);
        return -1;
    }
    return 0;
}

static int create_output(const char *path, int force, struct output_file *output)
{
    size_t length = strlen(path) + sizeof(".tmp.XXXXXX");

    memset(output, 0, sizeof(*output));
    output->fd = -1;
    output->temporary_path = malloc(length);
    if (output->temporary_path == NULL) {
        fprintf(stderr, "filecrypt: out of memory\n");
        return -1;
    }
    snprintf(output->temporary_path, length, "%s.tmp.XXXXXX", path);
    output->fd = mkstemp(output->temporary_path);
    if (output->fd == -1) {
        fprintf(stderr, "filecrypt: cannot create temporary output: %s\n",
                strerror(errno));
        free(output->temporary_path);
        output->temporary_path = NULL;
        return -1;
    }
    (void)fcntl(output->fd, F_SETFD, FD_CLOEXEC);
    if (fchmod(output->fd, S_IRUSR | S_IWUSR) == -1) {
        fprintf(stderr, "filecrypt: cannot set output permissions: %s\n",
                strerror(errno));
        abandon_output(output);
        return -1;
    }
    output->final_path = path;
    output->force = force;
    return 0;
}

static void abandon_output(struct output_file *output)
{
    if (output->fd != -1)
        close(output->fd);
    if (output->temporary_path != NULL) {
        unlink(output->temporary_path);
        free(output->temporary_path);
    }
    output->fd = -1;
    output->temporary_path = NULL;
}

static int commit_output(struct output_file *output)
{
    int result;

    if (fsync(output->fd) == -1) {
        fprintf(stderr, "filecrypt: cannot flush output: %s\n", strerror(errno));
        return -1;
    }
    if (close(output->fd) == -1) {
        output->fd = -1;
        fprintf(stderr, "filecrypt: cannot close output: %s\n", strerror(errno));
        return -1;
    }
    output->fd = -1;

    if (output->force) {
        result = rename(output->temporary_path, output->final_path);
    } else {
        /* link() fails if another process created the destination meanwhile. */
        result = link(output->temporary_path, output->final_path);
        if (result == 0)
            result = unlink(output->temporary_path);
    }
    if (result == -1) {
        fprintf(stderr, "filecrypt: cannot install output '%s': %s\n",
                output->final_path, strerror(errno));
        return -1;
    }
    free(output->temporary_path);
    output->temporary_path = NULL;
    return 0;
}

static void build_header(unsigned char header[HEADER_SIZE],
                         const unsigned char salt[SALT_SIZE],
                         const unsigned char nonce[NONCE_SIZE],
                         uint32_t iterations)
{
    uint32_t network_iterations = htonl(iterations);
    size_t offset = 0;

    memcpy(header + offset, MAGIC, MAGIC_SIZE);
    offset += MAGIC_SIZE;
    header[offset++] = VERSION;
    memcpy(header + offset, &network_iterations, sizeof(network_iterations));
    offset += sizeof(network_iterations);
    memcpy(header + offset, salt, SALT_SIZE);
    offset += SALT_SIZE;
    memcpy(header + offset, nonce, NONCE_SIZE);
}

static int parse_header(const unsigned char header[HEADER_SIZE],
                        unsigned char salt[SALT_SIZE],
                        unsigned char nonce[NONCE_SIZE],
                        uint32_t *iterations)
{
    uint32_t network_iterations;
    size_t offset = 0;

    if (memcmp(header, MAGIC, MAGIC_SIZE) != 0) {
        fprintf(stderr, "filecrypt: input is not a filecrypt file\n");
        return -1;
    }
    offset += MAGIC_SIZE;
    if (header[offset++] != VERSION) {
        fprintf(stderr, "filecrypt: unsupported filecrypt format version\n");
        return -1;
    }
    memcpy(&network_iterations, header + offset, sizeof(network_iterations));
    offset += sizeof(network_iterations);
    *iterations = ntohl(network_iterations);
    if (*iterations < 100000U || *iterations > 10000000U) {
        fprintf(stderr, "filecrypt: invalid key-derivation iteration count\n");
        return -1;
    }
    memcpy(salt, header + offset, SALT_SIZE);
    offset += SALT_SIZE;
    memcpy(nonce, header + offset, NONCE_SIZE);
    return 0;
}

int encrypt_file(const char *input_path,
                 const char *output_path,
                 const unsigned char *password,
                 int force,
                 const volatile sig_atomic_t *cancelled)
{
    int input_fd = -1;
    struct output_file output = {.fd = -1};
    EVP_CIPHER_CTX *context = NULL;
    unsigned char salt[SALT_SIZE];
    unsigned char nonce[NONCE_SIZE];
    unsigned char key[KEY_SIZE] = {0};
    unsigned char header[HEADER_SIZE];
    unsigned char input_buffer[BUFFER_SIZE];
    unsigned char output_buffer[BUFFER_SIZE + EVP_MAX_BLOCK_LENGTH];
    unsigned char tag[TAG_SIZE];
    ssize_t input_length;
    int output_length;
    int final_length;
    int status = EXIT_FAILURE;
    int key_locked = 0;

    if (open_input(input_path, output_path, force, &input_fd) == -1)
        goto cleanup;
    if (create_output(output_path, force, &output) == -1)
        goto cleanup;
    if (RAND_bytes(salt, sizeof(salt)) != 1 ||
        RAND_bytes(nonce, sizeof(nonce)) != 1) {
        report_openssl_error("secure random generation failed");
        goto cleanup;
    }
    if (mlock(key, sizeof(key)) == 0)
        key_locked = 1;
    if (derive_key(password, salt, PBKDF2_ITERATIONS, key) == -1)
        goto cleanup;

    build_header(header, salt, nonce, PBKDF2_ITERATIONS);
    if (write_all(output.fd, header, sizeof(header), cancelled) == -1) {
        fprintf(stderr, "filecrypt: cannot write output: %s\n", strerror(errno));
        goto cleanup;
    }

    context = EVP_CIPHER_CTX_new();
    /* Authenticate the header so its format parameters cannot be changed. */
    if (context == NULL ||
        EVP_EncryptInit_ex(context, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1 ||
        EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_IVLEN, NONCE_SIZE, NULL) != 1 ||
        EVP_EncryptInit_ex(context, NULL, NULL, key, nonce) != 1 ||
        EVP_EncryptUpdate(context, NULL, &output_length,
                          header, sizeof(header)) != 1) {
        report_openssl_error("could not initialize encryption");
        goto cleanup;
    }

    while ((input_length = read_retry(input_fd, input_buffer,
                                      sizeof(input_buffer), cancelled)) > 0) {
        if (EVP_EncryptUpdate(context, output_buffer, &output_length,
                              input_buffer, (int)input_length) != 1) {
            report_openssl_error("encryption failed");
            goto cleanup;
        }
        if (write_all(output.fd, output_buffer, (size_t)output_length,
                      cancelled) == -1) {
            fprintf(stderr, "filecrypt: cannot write output: %s\n", strerror(errno));
            goto cleanup;
        }
    }
    if (input_length == -1) {
        fprintf(stderr, "filecrypt: cannot read input: %s\n", strerror(errno));
        goto cleanup;
    }
    if (EVP_EncryptFinal_ex(context, output_buffer, &final_length) != 1 ||
        EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_GET_TAG, TAG_SIZE, tag) != 1) {
        report_openssl_error("could not finalize encryption");
        goto cleanup;
    }
    if (write_all(output.fd, output_buffer, (size_t)final_length, cancelled) == -1 ||
        write_all(output.fd, tag, sizeof(tag), cancelled) == -1) {
        fprintf(stderr, "filecrypt: cannot write output: %s\n", strerror(errno));
        goto cleanup;
    }
    if (commit_output(&output) == -1)
        goto cleanup;

    printf("Encrypted '%s' -> '%s'\n", input_path, output_path);
    status = EXIT_SUCCESS;

cleanup:
    if (*cancelled)
        fprintf(stderr, "filecrypt: interrupted; incomplete output removed\n");
    if (input_fd != -1)
        close(input_fd);
    if (output.temporary_path != NULL)
        abandon_output(&output);
    EVP_CIPHER_CTX_free(context);
    /* Erase sensitive buffers even when encryption fails. */
    OPENSSL_cleanse(key, sizeof(key));
    OPENSSL_cleanse(input_buffer, sizeof(input_buffer));
    OPENSSL_cleanse(output_buffer, sizeof(output_buffer));
    if (key_locked)
        (void)munlock(key, sizeof(key));
    return status;
}

int decrypt_file(const char *input_path,
                 const char *output_path,
                 const unsigned char *password,
                 int force,
                 const volatile sig_atomic_t *cancelled)
{
    int input_fd = -1;
    struct output_file output = {.fd = -1};
    struct stat input_stat;
    EVP_CIPHER_CTX *context = NULL;
    unsigned char salt[SALT_SIZE];
    unsigned char nonce[NONCE_SIZE];
    unsigned char key[KEY_SIZE] = {0};
    unsigned char header[HEADER_SIZE];
    unsigned char tag[TAG_SIZE];
    unsigned char input_buffer[BUFFER_SIZE];
    unsigned char output_buffer[BUFFER_SIZE + EVP_MAX_BLOCK_LENGTH];
    uint32_t iterations;
    off_t ciphertext_remaining;
    ssize_t input_length;
    size_t wanted;
    int output_length;
    int final_length;
    int status = EXIT_FAILURE;
    int key_locked = 0;

    if (open_input(input_path, output_path, force, &input_fd) == -1)
        goto cleanup;
    if (fstat(input_fd, &input_stat) == -1) {
        fprintf(stderr, "filecrypt: cannot inspect encrypted input: %s\n",
                strerror(errno));
        goto cleanup;
    }
    if (input_stat.st_size < (off_t)(HEADER_SIZE + TAG_SIZE)) {
        fprintf(stderr, "filecrypt: encrypted input is too short\n");
        goto cleanup;
    }
    if (read_exact(input_fd, header, sizeof(header), cancelled) == -1) {
        if (!*cancelled)
            fprintf(stderr, "filecrypt: cannot read encrypted header\n");
        goto cleanup;
    }
    if (parse_header(header, salt, nonce, &iterations) == -1)
        goto cleanup;

    if (lseek(input_fd, -TAG_SIZE, SEEK_END) == (off_t)-1 ||
        read_exact(input_fd, tag, sizeof(tag), cancelled) == -1 ||
        lseek(input_fd, HEADER_SIZE, SEEK_SET) == (off_t)-1) {
        fprintf(stderr, "filecrypt: cannot read encrypted input: %s\n",
                strerror(errno));
        goto cleanup;
    }
    ciphertext_remaining = input_stat.st_size - HEADER_SIZE - TAG_SIZE;

    if (create_output(output_path, force, &output) == -1)
        goto cleanup;
    if (mlock(key, sizeof(key)) == 0)
        key_locked = 1;
    if (derive_key(password, salt, iterations, key) == -1)
        goto cleanup;

    context = EVP_CIPHER_CTX_new();
    /* Authenticate the same header before accepting any plaintext. */
    if (context == NULL ||
        EVP_DecryptInit_ex(context, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1 ||
        EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_IVLEN, NONCE_SIZE, NULL) != 1 ||
        EVP_DecryptInit_ex(context, NULL, NULL, key, nonce) != 1 ||
        EVP_DecryptUpdate(context, NULL, &output_length,
                          header, sizeof(header)) != 1) {
        report_openssl_error("could not initialize decryption");
        goto cleanup;
    }

    while (ciphertext_remaining > 0) {
        wanted = ciphertext_remaining > (off_t)sizeof(input_buffer)
                     ? sizeof(input_buffer)
                     : (size_t)ciphertext_remaining;
        input_length = read_retry(input_fd, input_buffer, wanted, cancelled);
        if (input_length <= 0) {
            fprintf(stderr, "filecrypt: encrypted input ended unexpectedly\n");
            goto cleanup;
        }
        ciphertext_remaining -= input_length;
        if (EVP_DecryptUpdate(context, output_buffer, &output_length,
                              input_buffer, (int)input_length) != 1) {
            report_openssl_error("decryption failed");
            goto cleanup;
        }
        if (write_all(output.fd, output_buffer, (size_t)output_length,
                      cancelled) == -1) {
            fprintf(stderr, "filecrypt: cannot write output: %s\n", strerror(errno));
            goto cleanup;
        }
    }

    if (EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_TAG, TAG_SIZE, tag) != 1) {
        report_openssl_error("could not set authentication tag");
        goto cleanup;
    }
    /* Do not install plaintext until GCM verifies the complete ciphertext. */
    if (EVP_DecryptFinal_ex(context, output_buffer, &final_length) != 1) {
        fprintf(stderr,
                "filecrypt: authentication failed (wrong password or damaged file)\n");
        goto cleanup;
    }
    if (write_all(output.fd, output_buffer, (size_t)final_length, cancelled) == -1) {
        fprintf(stderr, "filecrypt: cannot write output: %s\n", strerror(errno));
        goto cleanup;
    }
    if (commit_output(&output) == -1)
        goto cleanup;

    printf("Decrypted '%s' -> '%s'\n", input_path, output_path);
    status = EXIT_SUCCESS;

cleanup:
    if (*cancelled)
        fprintf(stderr, "filecrypt: interrupted; incomplete output removed\n");
    if (input_fd != -1)
        close(input_fd);
    if (output.temporary_path != NULL)
        abandon_output(&output);
    EVP_CIPHER_CTX_free(context);
    OPENSSL_cleanse(key, sizeof(key));
    OPENSSL_cleanse(input_buffer, sizeof(input_buffer));
    OPENSSL_cleanse(output_buffer, sizeof(output_buffer));
    if (key_locked)
        (void)munlock(key, sizeof(key));
    return status;
}
