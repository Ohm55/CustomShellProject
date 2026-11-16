#ifndef FILECRYPT_CRYPTO_H
#define FILECRYPT_CRYPTO_H

#include <signal.h>

/* Encryption checks cancellation during I/O and does not install partial output. */
int encrypt_file(const char *input_path,
                 const char *output_path,
                 const unsigned char *password,
                 int force,
                 const volatile sig_atomic_t *cancelled);

/* Decryption authenticates the input before installing the recovered file. */
int decrypt_file(const char *input_path,
                 const char *output_path,
                 const unsigned char *password,
                 int force,
                 const volatile sig_atomic_t *cancelled);

#endif
