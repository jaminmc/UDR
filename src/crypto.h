/*****************************************************************************
Copyright 2012 Laboratory for Advanced Computing at the University of Chicago

This file is part of UDR.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing,
software distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions
and limitations under the License.
*****************************************************************************/
#ifndef CRYPTO_H
#define CRYPTO_H

#define PASSPHRASE_SIZE 32
#define HEX_PASSPHRASE_SIZE 64
#define EVP_ENCRYPT 1
#define EVP_DECRYPT 0
#define CTR_MODE 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/err.h>
#include <openssl/opensslv.h>
#include <openssl/crypto.h>
#include <limits.h>
#include <iostream>
//#include "udr_log.h"

using namespace std;

// AES-CTR is preferred: stream-friendly and uses CPU AES (AES-NI / ARMv8 CE)
// through OpenSSL's default EVP implementation when available.
#if !defined(OPENSSL_HAS_CTR) && (OPENSSL_VERSION_NUMBER >= 0x10001000L)
#define OPENSSL_HAS_CTR 1
#endif

class crypto
{
    private:
    //BF_KEY key;
    unsigned char ivec[ 1024 ];
    int direction;

    int passphrase_size;
    int hex_passphrase_size;

    // EVP stuff
    EVP_CIPHER_CTX *ctx;

    static void ensure_openssl_init()
    {
        static int once = 0;
        if (once)
            return;
        once = 1;
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
        // Load default provider/algorithms; AES-NI / ARMv8 CE used automatically
        OPENSSL_init_crypto(OPENSSL_INIT_LOAD_CRYPTO_STRINGS |
                            OPENSSL_INIT_ADD_ALL_CIPHERS, NULL);
#else
        OpenSSL_add_all_algorithms();
#endif
    }

    public:

    crypto(int direc, int len, unsigned char* password, char *encryption_type)
    {
        //free_key( password ); can't free here because is reused by threads
        const EVP_CIPHER *cipher;

        ensure_openssl_init();

        //aes-128|aes-192|aes-256|bf|des-ede3
        if (strncmp("aes-128", encryption_type, 8) == 0) {
#ifdef OPENSSL_HAS_CTR
            if (CTR_MODE)
                cipher = EVP_aes_128_ctr();
            else
#endif
                cipher = EVP_aes_128_cfb();
        }
        else if (strncmp("aes-192", encryption_type, 8) == 0) {
#ifdef OPENSSL_HAS_CTR
            if (CTR_MODE)
                cipher = EVP_aes_192_ctr();
            else
#endif
                cipher = EVP_aes_192_cfb();
        }
        else if (strncmp("aes-256", encryption_type, 8) == 0) {
#ifdef OPENSSL_HAS_CTR
            if (CTR_MODE)
                // OpenSSL dispatches to AES-NI (x86) or ARMv8 CE when present
                cipher = EVP_aes_256_ctr();
            else
#endif
                cipher = EVP_aes_256_cfb();
        }
        else if (strncmp("des-ede3", encryption_type, 9) == 0) {
            // apparently there is no 3des nor bf ctr
            cipher = EVP_des_ede3_cfb();
        }
        else if (strncmp("bf", encryption_type, 3) == 0) {
            cipher = EVP_bf_cfb();
        }
        else {
            fprintf(stderr, "error unsupported encryption type %s\n",
                encryption_type);
            exit(EXIT_FAILURE);
        }

        memset(ivec, 0, 1024);

        direction = direc;
        // EVP stuff — NULL impl uses OpenSSL default (hardware-accelerated AES)
        ctx = EVP_CIPHER_CTX_new();
        if (!ctx) {
            fprintf(stderr, "error allocating cipher context\n");
            exit(EXIT_FAILURE);
        }

        if (!EVP_CipherInit_ex(ctx, cipher, NULL, password, ivec, direc)) {
            fprintf(stderr, "error setting encryption scheme\n");
            exit(EXIT_FAILURE);
        }
    }

    ~crypto()
    {
        EVP_CIPHER_CTX_free(ctx);
    }

    // Returns how much has been encrypted and will call encrypt final when
    // given len of 0
    int encrypt(char *in, char *out, int len)
    {
        int evp_outlen;

        if (len == 0) {
            if (!EVP_CipherFinal_ex(ctx, (unsigned char *)out, &evp_outlen)) {
                fprintf(stderr, "encryption error\n");
                exit(EXIT_FAILURE);
            }
            return evp_outlen;
        }

        if(!EVP_CipherUpdate(ctx, (unsigned char *)out, &evp_outlen, (unsigned char *)in, len))
        {
            fprintf(stderr, "encryption error\n");
            exit(EXIT_FAILURE);
        }
        return evp_outlen;
    }
};

#endif
