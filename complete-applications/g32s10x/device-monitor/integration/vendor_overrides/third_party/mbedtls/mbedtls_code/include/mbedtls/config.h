/*
 *  Minimal configuration for TLS 1.1 (RFC 4346)
 *
 *  Copyright (C) 2006-2015, ARM Limited, All Rights Reserved
 *  SPDX-License-Identifier: Apache-2.0
 *
 *  Licensed under the Apache License, Version 2.0 (the "License"); you may
 *  not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 *  WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 *  This file is part of mbed TLS (https://tls.mbed.org)
 */
/*
 * Minimal configuration for TLS 1.1 (RFC 4346), implementing only the
 * required ciphersuite: MBEDTLS_TLS_RSA_WITH_3DES_EDE_CBC_SHA
 *
 * See README.txt for usage instructions.
 */

#ifndef __MBEDTLS_CONFIG_H__
#define __MBEDTLS_CONFIG_H__

#include "sys/types.h"
#ifdef JIELI_5713_RTOS
#include "generic/printf.h"
#endif

#define MBEDTLS_SSL_SESSION_TICKETS

/* mbed TLS feature support */
#define MBEDTLS_CIPHER_MODE_CBC
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_KEY_EXCHANGE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_SSL_PROTO_TLS1
#define MBEDTLS_SSL_PROTO_TLS1_1
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_RENEGOTIATION

/* mbed TLS modules */
#define MBEDTLS_AES_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_DES_C
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_MD_C
#define MBEDTLS_MD5_C
#define MBEDTLS_NET_C
#define MBEDTLS_OID_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_RSA_C
#define MBEDTLS_SHA1_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_SRV_C //MODIFY BY SHUNJIAN

#define MBEDTLS_SSL_TICKET_C
#define MBEDTLS_GCM_C

#define MBEDTLS_SSL_CACHE_C
#define MBEDTLS_SSL_PROTO_DTLS
#define MBEDTLS_TIMING_C

#define MBEDTLS_SSL_COOKIE_C
#define MBEDTLS_SSL_DTLS_HELLO_VERIFY

#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_USE_C

/* For test certificates */
#define MBEDTLS_BASE64_C
#define MBEDTLS_CERTS_C
#define MBEDTLS_PEM_PARSE_C

/* For testing with compat.sh */
// #define MBEDTLS_FS_IO //MODIFY BY SHUNJIAN



#define MBEDTLS_SSL_SERVER_NAME_INDICATION  // MODIFY BY YZQ

#define mbedtls_time    time
#define mbedtls_time_t  time_t
#define mbedtls_fprintf fprintf
#define mbedtls_printf  printf
#define MBEDTLS_SSL_MAX_FRAGMENT_LENGTH
#define MBEDTLS_CIPHER_MODE_CTR
#define MBEDTLS_KEY_EXCHANGE_PSK_ENABLED

// #define MBEDTLS_AES_SETKEY_ENC_ALT
// #define MBEDTLS_AES_SETKEY_DEC_ALT
// #define MBEDTLS_AES_ENCRYPT_ALT
// #define MBEDTLS_AES_DECRYPT_ALT
#define MBEDTLS_SELF_TEST  // MODIFY BY SHUNJIAN
// #define MBEDTLS_SHA1_PROCESS_ALT
//  #define MBEDTLS_SHA256_PROCESS_ALT
//  #define MBEDTLS_SSL_EXPORT_KEYS
#define MBEDTLS_CIPHER_TLS_RSA_WITH_AES_256_CBC_SHA256
// #define MBEDTLS_CIPHER_TLS_RSA_WITH_AES_256_CBC_SHA

/*     MODIFY BY XINQIAO  start       */
#define MBEDTLS_ECP_DP_SECP192R1_ENABLED
#define MBEDTLS_ECP_DP_SECP224R1_ENABLED
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_SECP521R1_ENABLED
#define MBEDTLS_ECP_DP_SECP192K1_ENABLED
#define MBEDTLS_ECP_DP_SECP224K1_ENABLED
#define MBEDTLS_ECP_DP_SECP256K1_ENABLED
#define MBEDTLS_ECP_DP_BP256R1_ENABLED
#define MBEDTLS_ECP_DP_BP384R1_ENABLED
#define MBEDTLS_ECP_DP_BP512R1_ENABLED
/* Keep X25519 disabled until target-side ECDH compatibility is proven. */
/*
 * Keep the context layout used by the vendor's full 2.25 configuration.
 * The smaller experimental variant is not reliable on this G32 port.
 */
#define MBEDTLS_ECDH_LEGACY_CONTEXT

#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDH_C

// #使用自定义的malloc 和free函数
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY

// #define MBEDTLS_DEBUG_C
#define MBEDTLS_ERROR_C
// #define MBEDTLS_ERROR_STRERROR_DUMMY

/*     MODIFY BY XINQIAO  end         */

#include "mbedtls/check_config.h"

#endif  //__MBEDTLS_CONFIG_H__
