/*
 *  Elliptic curves over GF(p): generic functions
 *
 *  Copyright The Mbed TLS Contributors
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
 */

/*
 * References:
 *
 * SEC1 http://www.secg.org/index.php?action=secg,docs_secg
 * GECC = Guide to Elliptic Curve Cryptography - Hankerson, Menezes, Vanstone
 * FIPS 186-3 http://csrc.nist.gov/publications/fips/fips186-3/fips_186-3.pdf
 * RFC 4492 for the related TLS structures and constants
 * RFC 7748 for the Curve448 and Curve25519 curve definitions
 *
 * [Curve25519] http://cr.yp.to/ecdh/curve25519-20060209.pdf
 *
 * [2] CORON, Jean-S'ebastien. Resistance against differential power analysis
 *     for elliptic curve cryptosystems. In : Cryptographic Hardware and
 *     Embedded Systems. Springer Berlin Heidelberg, 1999. p. 292-302.
 *     <http://link.springer.com/chapter/10.1007/3-540-48059-5_25>
 *
 * [3] HEDABOU, Mustapha, PINEL, Pierre, et B'EN'ETEAU, Lucien. A comb method to
 *     render ECC resistant against Side Channel Attacks. IACR Cryptology
 *     ePrint Archive, 2004, vol. 2004, p. 342.
 *     <http://eprint.iacr.org/2004/342.pdf>
 */


/**
 * \brief Function level alternative implementation.
 *
 * The MBEDTLS_ECP_INTERNAL_ALT macro enables alternative implementations to
 * replace certain functions in this module. The alternative implementations are
 * typically hardware accelerators and need to activate the hardware before the
 * computation starts and deactivate it after it finishes. The
 * mbedtls_internal_ecp_init() and mbedtls_internal_ecp_free() functions serve
 * this purpose.
 *
 * To preserve the correct functionality the following conditions must hold:
 *
 * - The alternative implementation must be activated by
 *   mbedtls_internal_ecp_init() before any of the replaceable functions is
 *   called.
 * - mbedtls_internal_ecp_free() must \b only be called when the alternative
 *   implementation is activated.
 * - mbedtls_internal_ecp_init() must \b not be called when the alternative
 *   implementation is activated.
 * - Public functions must not return while the alternative implementation is
 *   activated.
 * - Replaceable functions are guarded by \c MBEDTLS_ECP_XXX_ALT macros and
 *   before calling them an \code if( mbedtls_internal_ecp_grp_capable( grp ) )
 *   \endcode ensures that the alternative implementation supports the current
 *   group.
 */
#include <string.h>
#include "atbm_hal.h"
#include "ecp.h"
#include "ecp_extend.h"

/* The ATBM ECP header predates the curve-family feature flags used below. */
#if !defined(MBEDTLS_ECP_SHORT_WEIERSTRASS_ENABLED)
#define MBEDTLS_ECP_SHORT_WEIERSTRASS_ENABLED
#endif

typedef struct
{
    int (*f_rng)( void *, unsigned char *, size_t );
    void *p_rng;
} atbm_ecp_rng_bridge_context;

static int atbm_ecp_rng_bridge( void *ctx, unsigned char *output,
                                atbm_size_t output_len )
{
    atbm_ecp_rng_bridge_context *bridge = ctx;

    if( bridge == NULL || bridge->f_rng == NULL || output_len < 0 )
        return( MBEDTLS_ERR_ECP_BAD_INPUT_DATA );

    return( bridge->f_rng( bridge->p_rng, output, (size_t) output_len ) );
}

static int atbm_ecp_mpi_fill_random( atbm_mbedtls_mpi *value, size_t size,
                                     int (*f_rng)( void *, unsigned char *, size_t ),
                                     void *p_rng )
{
    atbm_ecp_rng_bridge_context bridge;

    if( size > 0x7FFFFFFFUL || f_rng == NULL )
        return( MBEDTLS_ERR_ECP_BAD_INPUT_DATA );

    bridge.f_rng = f_rng;
    bridge.p_rng = p_rng;
    return( atbm_mbedtls_mpi_fill_random( value, (atbm_size_t) size,
                                          atbm_ecp_rng_bridge, &bridge ) );
}


#if !defined(MBEDTLS_ECP_ALT)

#define ATBM_MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED -0x006E  /**< This is a bug in the library */

/* Parameter validation macros based on platform_util.h */
#define ECP_VALIDATE_RET( cond )    \
    MBEDTLS_INTERNAL_VALIDATE_RET( cond, MBEDTLS_ERR_ECP_BAD_INPUT_DATA )
#define ECP_VALIDATE( cond )        \
    MBEDTLS_INTERNAL_VALIDATE( cond )

#if defined(MBEDTLS_PLATFORM_C)
#include "mbedtls/platform.h"
#else
#include <stdlib.h>
#ifndef JIELI_5713_RTOS
#include <stdio.h>
#endif
#define mbedtls_printf     printf
#define mbedtls_calloc    calloc
#define mbedtls_free       free
#endif

#include "ecp_internal.h"

#if !defined(MBEDTLS_ECP_NO_INTERNAL_RNG)
#if defined(MBEDTLS_HMAC_DRBG_C)
#include "mbedtls/hmac_drbg.h"
#elif defined(MBEDTLS_CTR_DRBG_C)
#include "mbedtls/ctr_drbg.h"
#else
#endif
#endif /* MBEDTLS_ECP_NO_INTERNAL_RNG */

#if ( defined(__ARMCC_VERSION) || defined(_MSC_VER) ) && \
    !defined(inline) && !defined(__cplusplus)
#define inline __inline
#endif

#if defined(MBEDTLS_SELF_TEST)
/*
 * Counts of point addition and doubling, and field multiplications.
 * Used to test resistance of point multiplication to simple timing attacks.
 */
static unsigned long add_count, dbl_count, mul_count;
#endif


#if defined(MBEDTLS_SELF_TEST)
#define INC_MUL_COUNT   mul_count++;
#else
#define INC_MUL_COUNT
#endif
#define MOD_MUL( N )                                                    \
    do                                                                  \
    {                                                                   \
        MBEDTLS_MPI_CHK( ecp_modp( &(N), grp ) );                       \
        INC_MUL_COUNT                                                   \
    } while( 0 )


/*
 * Reduce a atbm_mbedtls_mpi mod p in-place, to use after atbm_mbedtls_mpi_sub_mpi
 * N->s < 0 is a very fast test, which fails only if N is 0
 */
#define MOD_SUB( N )                                                    \
    while( (N).s < 0 && atbm_mbedtls_mpi_cmp_int( &(N), 0 ) != 0 )           \
        MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_add_mpi( &(N), &(N), &grp->P ) )


/*
 * Reduce a atbm_mbedtls_mpi mod p in-place, to use after atbm_mbedtls_mpi_add_mpi and atbm_mbedtls_mpi_mul_int.
 * We known P, N and the result are positive, so sub_abs is correct, and
 * a bit faster.
 */
#define MOD_ADD( N )                                                    \
    while( atbm_mbedtls_mpi_cmp_mpi( &(N), &grp->P ) >= 0 )                  \
        MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_sub_abs( &(N), &(N), &grp->P ) )
static inline int atbm_mbedtls_mpi_add_mod( const mbedtls_ecp_group *grp,
                                       atbm_mbedtls_mpi *X,
                                       const atbm_mbedtls_mpi *A,
                                       const atbm_mbedtls_mpi *B )
{
    int ret = ATBM_MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_add_mpi( X, A, B ) );
    MOD_ADD( *X );
cleanup:
    return( ret );
}
/* d = ceil( n / w ) */
#define COMB_MAX_D      ( MBEDTLS_ECP_MAX_BITS + 1 ) / 2

/* number of precomputed points */
#define COMB_MAX_PRE    ( 1 << ( MBEDTLS_ECP_WINDOW_SIZE - 1 ) )

static inline int atbm_mbedtls_mpi_sub_mod( const mbedtls_ecp_group *grp,
                                       atbm_mbedtls_mpi *X,
                                       const atbm_mbedtls_mpi *A,
                                       const atbm_mbedtls_mpi *B )
{
    int ret = ATBM_MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_sub_mpi( X, A, B ) );
    MOD_SUB( *X );
cleanup:
    return( ret );
}
static inline int atbm_mbedtls_mpi_shift_l_mod( const mbedtls_ecp_group *grp,
                                           atbm_mbedtls_mpi *X,
                                           size_t count )
{
    int ret = ATBM_MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_shift_l( X, count ) );
    MOD_ADD( *X );
cleanup:
    return( ret );
}
static inline int atbm_mbedtls_mpi_mul_mod( const mbedtls_ecp_group *grp,
                                       atbm_mbedtls_mpi *X,
                                       const atbm_mbedtls_mpi *A,
                                       const atbm_mbedtls_mpi *B )
{
    int ret = ATBM_MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_mul_mpi( X, A, B ) );
    MOD_MUL( *X );
cleanup:
    return( ret );
}
static int ecp_double_jac( const mbedtls_ecp_group *grp, mbedtls_ecp_point *R,
                           const mbedtls_ecp_point *P )
{
    int ret = ATBM_MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;
    atbm_mbedtls_mpi M, S, T, U;

#if defined(MBEDTLS_SELF_TEST)
    dbl_count++;
#endif

#if defined(MBEDTLS_ECP_DOUBLE_JAC_ALT)
    if( mbedtls_internal_ecp_grp_capable( grp ) )
        return( mbedtls_internal_ecp_double_jac( grp, R, P ) );
#endif /* MBEDTLS_ECP_DOUBLE_JAC_ALT */

    atbm_mbedtls_mpi_init( &M ); atbm_mbedtls_mpi_init( &S ); atbm_mbedtls_mpi_init( &T ); atbm_mbedtls_mpi_init( &U );

    /* Special case for A = -3 */
    if( grp->A.p == NULL )
    {
        /* M = 3(X + Z^2)(X - Z^2) */
        MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_mul_mod( grp, &S,  &P->Z,  &P->Z   ) );
        MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_add_mod( grp, &T,  &P->X,  &S      ) );
        MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_sub_mod( grp, &U,  &P->X,  &S      ) );
        MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_mul_mod( grp, &S,  &T,     &U      ) );
        MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_mul_int( &M,  &S,     3       ) ); MOD_ADD( M );
    }
    else
    {
        /* M = 3.X^2 */
        MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_mul_mod( grp, &S,  &P->X,  &P->X   ) );
        MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_mul_int( &M,  &S,     3       ) ); MOD_ADD( M );

        /* Optimize away for "koblitz" curves with A = 0 */
        if( atbm_mbedtls_mpi_cmp_int( &grp->A, 0 ) != 0 )
        {
            /* M += A.Z^4 */
            MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_mul_mod( grp, &S,  &P->Z,  &P->Z   ) );
            MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_mul_mod( grp, &T,  &S,     &S      ) );
            MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_mul_mod( grp, &S,  &T,     &grp->A ) );
            MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_add_mod( grp, &M,  &M,     &S      ) );
        }
    }

    /* S = 4.X.Y^2 */
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_mul_mod( grp, &T,  &P->Y,  &P->Y   ) );
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_shift_l_mod( grp, &T,  1               ) );
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_mul_mod( grp, &S,  &P->X,  &T      ) );
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_shift_l_mod( grp, &S,  1               ) );

    /* U = 8.Y^4 */
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_mul_mod( grp, &U,  &T,     &T      ) );
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_shift_l_mod( grp, &U,  1               ) );

    /* T = M^2 - 2.S */
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_mul_mod( grp, &T,  &M,     &M      ) );
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_sub_mod( grp, &T,  &T,     &S      ) );
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_sub_mod( grp, &T,  &T,     &S      ) );

    /* S = M(S - T) - U */
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_sub_mod( grp, &S,  &S,     &T      ) );
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_mul_mod( grp, &S,  &S,     &M      ) );
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_sub_mod( grp, &S,  &S,     &U      ) );

    /* U = 2.Y.Z */
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_mul_mod( grp, &U,  &P->Y,  &P->Z   ) );
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_shift_l_mod( grp, &U,  1               ) );

    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_copy( &R->X, &T ) );
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_copy( &R->Y, &S ) );
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_copy( &R->Z, &U ) );

cleanup:
    atbm_mbedtls_mpi_free( &M ); atbm_mbedtls_mpi_free( &S ); atbm_mbedtls_mpi_free( &T ); atbm_mbedtls_mpi_free( &U );

    return( ret );
}
static int ecp_randomize_jac( const mbedtls_ecp_group *grp, mbedtls_ecp_point *pt,
                int (*f_rng)(void *, unsigned char *, size_t), void *p_rng )
{
    int ret = ATBM_MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;
    atbm_mbedtls_mpi l, ll;
    size_t p_size;
    int count = 0;

#if defined(MBEDTLS_ECP_RANDOMIZE_JAC_ALT)
    if( mbedtls_internal_ecp_grp_capable( grp ) )
        return( mbedtls_internal_ecp_randomize_jac( grp, pt, f_rng, p_rng ) );
#endif /* MBEDTLS_ECP_RANDOMIZE_JAC_ALT */

    p_size = ( grp->pbits + 7 ) / 8;
    atbm_mbedtls_mpi_init( &l ); atbm_mbedtls_mpi_init( &ll );

    /* Generate l such that 1 < l < p */
    do
    {
        MBEDTLS_MPI_CHK( atbm_ecp_mpi_fill_random( &l, p_size, f_rng, p_rng ) );

        while( atbm_mbedtls_mpi_cmp_mpi( &l, &grp->P ) >= 0 )
            MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_shift_r( &l, 1 ) );

        if( count++ > 10 )
        {
            ret = MBEDTLS_ERR_ECP_RANDOM_FAILED;
            goto cleanup;
        }
    }
    while( atbm_mbedtls_mpi_cmp_int( &l, 1 ) <= 0 );

    /* Z = l * Z */
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_mul_mod( grp, &pt->Z,   &pt->Z,     &l  ) );

    /* X = l^2 * X */
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_mul_mod( grp, &ll,      &l,         &l  ) );
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_mul_mod( grp, &pt->X,   &pt->X,     &ll ) );

    /* Y = l^3 * Y */
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_mul_mod( grp, &ll,      &ll,        &l  ) );
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_mul_mod( grp, &pt->Y,   &pt->Y,     &ll ) );

cleanup:
    atbm_mbedtls_mpi_free( &l ); atbm_mbedtls_mpi_free( &ll );

    return( ret );
}
static int ecp_add_mixed( const mbedtls_ecp_group *grp, mbedtls_ecp_point *R,
                          const mbedtls_ecp_point *P, const mbedtls_ecp_point *Q )
{
    int ret = ATBM_MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;
    atbm_mbedtls_mpi T1, T2, T3, T4, X, Y, Z;

#if defined(MBEDTLS_SELF_TEST)
    add_count++;
#endif

#if defined(MBEDTLS_ECP_ADD_MIXED_ALT)
    if( mbedtls_internal_ecp_grp_capable( grp ) )
        return( mbedtls_internal_ecp_add_mixed( grp, R, P, Q ) );
#endif /* MBEDTLS_ECP_ADD_MIXED_ALT */

    /*
     * Trivial cases: P == 0 or Q == 0 (case 1)
     */
    if( atbm_mbedtls_mpi_cmp_int( &P->Z, 0 ) == 0 )
        return( mbedtls_ecp_copy( R, Q ) );

    if( Q->Z.p != NULL && atbm_mbedtls_mpi_cmp_int( &Q->Z, 0 ) == 0 )
        return( mbedtls_ecp_copy( R, P ) );

    /*
     * Make sure Q coordinates are normalized
     */
    if( Q->Z.p != NULL && atbm_mbedtls_mpi_cmp_int( &Q->Z, 1 ) != 0 )
        return( MBEDTLS_ERR_ECP_BAD_INPUT_DATA );

    atbm_mbedtls_mpi_init( &T1 ); atbm_mbedtls_mpi_init( &T2 ); atbm_mbedtls_mpi_init( &T3 ); atbm_mbedtls_mpi_init( &T4 );
    atbm_mbedtls_mpi_init( &X ); atbm_mbedtls_mpi_init( &Y ); atbm_mbedtls_mpi_init( &Z );

    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_mul_mod( grp, &T1,  &P->Z,  &P->Z ) );
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_mul_mod( grp, &T2,  &T1,    &P->Z ) );
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_mul_mod( grp, &T1,  &T1,    &Q->X ) );
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_mul_mod( grp, &T2,  &T2,    &Q->Y ) );
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_sub_mod( grp, &T1,  &T1,    &P->X ) );
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_sub_mod( grp, &T2,  &T2,    &P->Y ) );

    /* Special cases (2) and (3) */
    if( atbm_mbedtls_mpi_cmp_int( &T1, 0 ) == 0 )
    {
        if( atbm_mbedtls_mpi_cmp_int( &T2, 0 ) == 0 )
        {
            ret = ecp_double_jac( grp, R, P );
            goto cleanup;
        }
        else
        {
            ret = mbedtls_ecp_set_zero( R );
            goto cleanup;
        }
    }

    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_mul_mod( grp, &Z,   &P->Z,  &T1   ) );
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_mul_mod( grp, &T3,  &T1,    &T1   ) );
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_mul_mod( grp, &T4,  &T3,    &T1   ) );
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_mul_mod( grp, &T3,  &T3,    &P->X ) );
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_copy( &T1, &T3 ) );
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_shift_l_mod( grp, &T1,  1     ) );
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_mul_mod( grp, &X,   &T2,    &T2   ) );
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_sub_mod( grp, &X,   &X,     &T1   ) );
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_sub_mod( grp, &X,   &X,     &T4   ) );
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_sub_mod( grp, &T3,  &T3,    &X    ) );
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_mul_mod( grp, &T3,  &T3,    &T2   ) );
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_mul_mod( grp, &T4,  &T4,    &P->Y ) );
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_sub_mod( grp, &Y,   &T3,    &T4   ) );

    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_copy( &R->X, &X ) );
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_copy( &R->Y, &Y ) );
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_copy( &R->Z, &Z ) );

cleanup:

    atbm_mbedtls_mpi_free( &T1 ); atbm_mbedtls_mpi_free( &T2 ); atbm_mbedtls_mpi_free( &T3 ); atbm_mbedtls_mpi_free( &T4 );
    atbm_mbedtls_mpi_free( &X ); atbm_mbedtls_mpi_free( &Y ); atbm_mbedtls_mpi_free( &Z );

    return( ret );
}
static int ecp_select_comb( const mbedtls_ecp_group *grp, mbedtls_ecp_point *R,
                            const mbedtls_ecp_point T[], unsigned char T_size,
                            unsigned char i )
{
    int ret = ATBM_MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;
    unsigned char ii, j;

    /* Ignore the "sign" bit and scale down */
    ii =  ( i & 0x7Fu ) >> 1;

    /* Read the whole table to thwart cache-based timing attacks */
    for( j = 0; j < T_size; j++ )
    {
        MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_safe_cond_assign( &R->X, &T[j].X, j == ii ) );
        MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_safe_cond_assign( &R->Y, &T[j].Y, j == ii ) );
    }

    /* Safely invert result if i is "negative" */
    MBEDTLS_MPI_CHK( ecp_safe_invert_jac( grp, R, i >> 7 ) );

cleanup:
    return( ret );
}
static int ecp_mul_comb_core( const mbedtls_ecp_group *grp, mbedtls_ecp_point *R,
                              const mbedtls_ecp_point T[], unsigned char T_size,
                              const unsigned char x[], size_t d,
                              int (*f_rng)(void *, unsigned char *, size_t),
                              void *p_rng,
                              void *rs_ctx )
{
    int ret = ATBM_MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;
    mbedtls_ecp_point Txi;
    size_t i;

    mbedtls_ecp_point_init( &Txi );

#if !defined(MBEDTLS_ECP_RESTARTABLE)
    (void) rs_ctx;
#endif

#if defined(MBEDTLS_ECP_RESTARTABLE)
    if( rs_ctx != NULL && rs_ctx->rsm != NULL &&
        rs_ctx->rsm->state != ecp_rsm_comb_core )
    {
        rs_ctx->rsm->i = 0;
        rs_ctx->rsm->state = ecp_rsm_comb_core;
    }

    /* new 'if' instead of nested for the sake of the 'else' branch */
    if( rs_ctx != NULL && rs_ctx->rsm != NULL && rs_ctx->rsm->i != 0 )
    {
        /* restore current index (R already pointing to rs_ctx->rsm->R) */
        i = rs_ctx->rsm->i;
    }
    else
#endif
    {
        /* Start with a non-zero point and randomize its coordinates */
        i = d;
        MBEDTLS_MPI_CHK( ecp_select_comb( grp, R, T, T_size, x[i] ) );
        MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_lset( &R->Z, 1 ) );
#if defined(MBEDTLS_ECP_NO_INTERNAL_RNG)
        if( f_rng != 0 )
#endif
            MBEDTLS_MPI_CHK( ecp_randomize_jac( grp, R, f_rng, p_rng ) );
    }

    while( i != 0 )
    {
        --i;

        MBEDTLS_MPI_CHK( ecp_double_jac( grp, R, R ) );
        MBEDTLS_MPI_CHK( ecp_select_comb( grp, &Txi, T, T_size, x[i] ) );
        MBEDTLS_MPI_CHK( ecp_add_mixed( grp, R, R, &Txi ) );
    }

cleanup:

    mbedtls_ecp_point_free( &Txi );

#if defined(MBEDTLS_ECP_RESTARTABLE)
    if( rs_ctx != NULL && rs_ctx->rsm != NULL &&
        ret == MBEDTLS_ERR_ECP_IN_PROGRESS )
    {
        rs_ctx->rsm->i = i;
        /* no need to save R, already pointing to rs_ctx->rsm->R */
    }
#endif

    return( ret );
}
static void ecp_comb_recode_core( unsigned char x[], size_t d,
                                  unsigned char w, const atbm_mbedtls_mpi *m )
{
    size_t i, j;
    unsigned char c, cc, adjust;

    memset( x, 0, d+1 );

    /* First get the classical comb values (except for x_d = 0) */
    for( i = 0; i < d; i++ )
        for( j = 0; j < w; j++ )
            x[i] |= atbm_mbedtls_mpi_get_bit( m, i + d * j ) << j;

    /* Now make sure x_1 .. x_d are odd */
    c = 0;
    for( i = 1; i <= d; i++ )
    {
        /* Add carry and update it */
        cc   = x[i] & c;
        x[i] = x[i] ^ c;
        c = cc;

        /* Adjust if needed, avoiding branches */
        adjust = 1 - ( x[i] & 0x01 );
        c   |= x[i] & ( x[i-1] * adjust );
        x[i] = x[i] ^ ( x[i-1] * adjust );
        x[i-1] |= adjust << 7;
    }
}
static int ecp_comb_recode_scalar( const mbedtls_ecp_group *grp,
                                   const atbm_mbedtls_mpi *m,
                                   unsigned char k[COMB_MAX_D + 1],
                                   size_t d,
                                   unsigned char w,
                                   unsigned char *parity_trick )
{
    int ret = ATBM_MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;
    atbm_mbedtls_mpi M, mm;

    atbm_mbedtls_mpi_init( &M );
    atbm_mbedtls_mpi_init( &mm );

    /* N is always odd (see above), just make extra sure */
    if( atbm_mbedtls_mpi_get_bit( &grp->N, 0 ) != 1 )
        return( MBEDTLS_ERR_ECP_BAD_INPUT_DATA );

    /* do we need the parity trick? */
    *parity_trick = ( atbm_mbedtls_mpi_get_bit( m, 0 ) == 0 );

    /* execute parity fix in constant time */
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_copy( &M, m ) );
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_sub_mpi( &mm, &grp->N, m ) );
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_safe_cond_assign( &M, &mm, *parity_trick ) );

    /* actual scalar recoding */
    ecp_comb_recode_core( k, d, w, &M );

cleanup:
    atbm_mbedtls_mpi_free( &mm );
    atbm_mbedtls_mpi_free( &M );

    return( ret );
}
static int ecp_normalize_jac( const mbedtls_ecp_group *grp, mbedtls_ecp_point *pt )
{
    int ret = ATBM_MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;
    atbm_mbedtls_mpi Zi, ZZi;

    if( atbm_mbedtls_mpi_cmp_int( &pt->Z, 0 ) == 0 )
        return( 0 );

#if defined(MBEDTLS_ECP_NORMALIZE_JAC_ALT)
    if( mbedtls_internal_ecp_grp_capable( grp ) )
        return( mbedtls_internal_ecp_normalize_jac( grp, pt ) );
#endif /* MBEDTLS_ECP_NORMALIZE_JAC_ALT */

    atbm_mbedtls_mpi_init( &Zi ); atbm_mbedtls_mpi_init( &ZZi );

    /*
     * X = X / Z^2  mod p
     */
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_inv_mod( &Zi,      &pt->Z,     &grp->P ) );
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_mul_mod( grp, &ZZi,     &Zi,        &Zi     ) );
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_mul_mod( grp, &pt->X,   &pt->X,     &ZZi    ) );

    /*
     * Y = Y / Z^3  mod p
     */
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_mul_mod( grp, &pt->Y,   &pt->Y,     &ZZi    ) );
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_mul_mod( grp, &pt->Y,   &pt->Y,     &Zi     ) );

    /*
     * Z = 1
     */
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_lset( &pt->Z, 1 ) );

cleanup:

    atbm_mbedtls_mpi_free( &Zi ); atbm_mbedtls_mpi_free( &ZZi );

    return( ret );
}
static int ecp_normalize_jac_many( const mbedtls_ecp_group *grp,
                                   mbedtls_ecp_point *T[], size_t T_size )
{
    int ret = ATBM_MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;
    size_t i;
    atbm_mbedtls_mpi *c, u, Zi, ZZi;

    if( T_size < 2 )
        return( ecp_normalize_jac( grp, *T ) );

#if defined(MBEDTLS_ECP_NORMALIZE_JAC_MANY_ALT)
    if( mbedtls_internal_ecp_grp_capable( grp ) )
        return( mbedtls_internal_ecp_normalize_jac_many( grp, T, T_size ) );
#endif

    if( ( c = mbedtls_calloc( T_size, sizeof( atbm_mbedtls_mpi ) ) ) == NULL )
        return( MBEDTLS_ERR_ECP_ALLOC_FAILED );

    for( i = 0; i < T_size; i++ )
        atbm_mbedtls_mpi_init( &c[i] );

    atbm_mbedtls_mpi_init( &u ); atbm_mbedtls_mpi_init( &Zi ); atbm_mbedtls_mpi_init( &ZZi );

    /*
     * c[i] = Z_0 * ... * Z_i
     */
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_copy( &c[0], &T[0]->Z ) );
    for( i = 1; i < T_size; i++ )
    {
        MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_mul_mod( grp, &c[i], &c[i-1], &T[i]->Z ) );
    }

    /*
     * u = 1 / (Z_0 * ... * Z_n) mod P
     */
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_inv_mod( &u, &c[T_size-1], &grp->P ) );

    for( i = T_size - 1; ; i-- )
    {
        /*
         * Zi = 1 / Z_i mod p
         * u = 1 / (Z_0 * ... * Z_i) mod P
         */
        if( i == 0 ) {
            MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_copy( &Zi, &u ) );
        }
        else
        {
            MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_mul_mod( grp, &Zi, &u, &c[i-1]  ) );
            MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_mul_mod( grp, &u,  &u, &T[i]->Z ) );
        }

        /*
         * proceed as in normalize()
         */
        MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_mul_mod( grp, &ZZi,     &Zi,      &Zi  ) );
        MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_mul_mod( grp, &T[i]->X, &T[i]->X, &ZZi ) );
        MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_mul_mod( grp, &T[i]->Y, &T[i]->Y, &ZZi ) );
        MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_mul_mod( grp, &T[i]->Y, &T[i]->Y, &Zi  ) );

        /*
         * Post-precessing: reclaim some memory by shrinking coordinates
         * - not storing Z (always 1)
         * - shrinking other coordinates, but still keeping the same number of
         *   limbs as P, as otherwise it will too likely be regrown too fast.
         */
        MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_shrink( &T[i]->X, grp->P.n ) );
        MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_shrink( &T[i]->Y, grp->P.n ) );
        atbm_mbedtls_mpi_free( &T[i]->Z );

        if( i == 0 )
            break;
    }

cleanup:

    atbm_mbedtls_mpi_free( &u ); atbm_mbedtls_mpi_free( &Zi ); atbm_mbedtls_mpi_free( &ZZi );
    for( i = 0; i < T_size; i++ )
        atbm_mbedtls_mpi_free( &c[i] );
    mbedtls_free( c );

    return( ret );
}


static int ecp_mul_comb_after_precomp( const mbedtls_ecp_group *grp,
                                mbedtls_ecp_point *R,
                                const atbm_mbedtls_mpi *m,
                                const mbedtls_ecp_point *T,
                                unsigned char T_size,
                                unsigned char w,
                                size_t d,
                                int (*f_rng)(void *, unsigned char *, size_t),
                                void *p_rng,
                                void *rs_ctx )
{
    int ret = ATBM_MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;
    unsigned char parity_trick;
    unsigned char k[COMB_MAX_D + 1];
    mbedtls_ecp_point *RR = R;

#if defined(MBEDTLS_ECP_RESTARTABLE)
    if( rs_ctx != NULL && rs_ctx->rsm != NULL )
    {
        RR = &rs_ctx->rsm->R;

        if( rs_ctx->rsm->state == ecp_rsm_final_norm )
            goto final_norm;
    }
#endif

    MBEDTLS_MPI_CHK( ecp_comb_recode_scalar( grp, m, k, d, w,
                                            &parity_trick ) );
    MBEDTLS_MPI_CHK( ecp_mul_comb_core( grp, RR, T, T_size, k, d,
                                        f_rng, p_rng, rs_ctx ) );
    MBEDTLS_MPI_CHK( ecp_safe_invert_jac( grp, RR, parity_trick ) );

#if defined(MBEDTLS_ECP_RESTARTABLE)
    if( rs_ctx != NULL && rs_ctx->rsm != NULL )
        rs_ctx->rsm->state = ecp_rsm_final_norm;

final_norm:
#endif
    /*
     * Knowledge of the jacobian coordinates may leak the last few bits of the
     * scalar [1], and since our MPI implementation isn't constant-flow,
     * inversion (used for coordinate normalization) may leak the full value
     * of its input via side-channels [2].
     *
     * [1] https://eprint.iacr.org/2003/191
     * [2] https://eprint.iacr.org/2020/055
     *
     * Avoid the leak by randomizing coordinates before we normalize them.
     */
#if defined(MBEDTLS_ECP_NO_INTERNAL_RNG)
    if( f_rng != 0 )
#endif
        MBEDTLS_MPI_CHK( ecp_randomize_jac( grp, RR, f_rng, p_rng ) );

    MBEDTLS_MPI_CHK( ecp_normalize_jac( grp, RR ) );

#if defined(MBEDTLS_ECP_RESTARTABLE)
    if( rs_ctx != NULL && rs_ctx->rsm != NULL )
        MBEDTLS_MPI_CHK( mbedtls_ecp_copy( R, RR ) );
#endif

cleanup:
    return( ret );
}


static int ecp_precompute_comb( const mbedtls_ecp_group *grp,
                                mbedtls_ecp_point T[], const mbedtls_ecp_point *P,
                                unsigned char w, size_t d,
                                void *rs_ctx )
{
    int ret = ATBM_MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;
    unsigned char i;
    size_t j = 0;
    const unsigned char T_size = 1U << ( w - 1 );
    mbedtls_ecp_point *cur, *TT[COMB_MAX_PRE - 1];

#if defined(MBEDTLS_ECP_RESTARTABLE)
    if( rs_ctx != NULL && rs_ctx->rsm != NULL )
    {
        if( rs_ctx->rsm->state == ecp_rsm_pre_dbl )
            goto dbl;
        if( rs_ctx->rsm->state == ecp_rsm_pre_norm_dbl )
            goto norm_dbl;
        if( rs_ctx->rsm->state == ecp_rsm_pre_add )
            goto add;
        if( rs_ctx->rsm->state == ecp_rsm_pre_norm_add )
            goto norm_add;
    }
#else
    (void) rs_ctx;
#endif

#if defined(MBEDTLS_ECP_RESTARTABLE)
    if( rs_ctx != NULL && rs_ctx->rsm != NULL )
    {
        rs_ctx->rsm->state = ecp_rsm_pre_dbl;

        /* initial state for the loop */
        rs_ctx->rsm->i = 0;
    }

dbl:
#endif
    /*
     * Set T[0] = P and
     * T[2^{l-1}] = 2^{dl} P for l = 1 .. w-1 (this is not the final value)
     */
    MBEDTLS_MPI_CHK( mbedtls_ecp_copy( &T[0], P ) );

#if defined(MBEDTLS_ECP_RESTARTABLE)
    if( rs_ctx != NULL && rs_ctx->rsm != NULL && rs_ctx->rsm->i != 0 )
        j = rs_ctx->rsm->i;
    else
#endif
        j = 0;

    for( ; j < d * ( w - 1 ); j++ )
    {

        i = 1U << ( j / d );
        cur = T + i;

        if( j % d == 0 )
            MBEDTLS_MPI_CHK( mbedtls_ecp_copy( cur, T + ( i >> 1 ) ) );

        MBEDTLS_MPI_CHK( ecp_double_jac( grp, cur, cur ) );
    }

#if defined(MBEDTLS_ECP_RESTARTABLE)
    if( rs_ctx != NULL && rs_ctx->rsm != NULL )
        rs_ctx->rsm->state = ecp_rsm_pre_norm_dbl;

norm_dbl:
#endif
    /*
     * Normalize current elements in T. As T has holes,
     * use an auxiliary array of pointers to elements in T.
     */
    j = 0;
    for( i = 1; i < T_size; i <<= 1 )
        TT[j++] = T + i;

    MBEDTLS_MPI_CHK( ecp_normalize_jac_many( grp, TT, j ) );

#if defined(MBEDTLS_ECP_RESTARTABLE)
    if( rs_ctx != NULL && rs_ctx->rsm != NULL )
        rs_ctx->rsm->state = ecp_rsm_pre_add;

add:
#endif
    /*
     * Compute the remaining ones using the minimal number of additions
     * Be careful to update T[2^l] only after using it!
     */

    for( i = 1; i < T_size; i <<= 1 )
    {
        j = i;
        while( j-- )
            MBEDTLS_MPI_CHK( ecp_add_mixed( grp, &T[i + j], &T[j], &T[i] ) );
    }

#if defined(MBEDTLS_ECP_RESTARTABLE)
    if( rs_ctx != NULL && rs_ctx->rsm != NULL )
        rs_ctx->rsm->state = ecp_rsm_pre_norm_add;

norm_add:
#endif
    /*
     * Normalize final elements in T. Even though there are no holes now, we
     * still need the auxiliary array for homogeneity with the previous
     * call. Also, skip T[0] which is already normalised, being a copy of P.
     */
    for( j = 0; j + 1 < T_size; j++ )
        TT[j] = T + j + 1;


    MBEDTLS_MPI_CHK( ecp_normalize_jac_many( grp, TT, j ) );

cleanup:
#if defined(MBEDTLS_ECP_RESTARTABLE)
    if( rs_ctx != NULL && rs_ctx->rsm != NULL &&
        ret == MBEDTLS_ERR_ECP_IN_PROGRESS )
    {
        if( rs_ctx->rsm->state == ecp_rsm_pre_dbl )
            rs_ctx->rsm->i = j;
    }
#endif

    return( ret );
}
static unsigned char ecp_pick_window_size( const mbedtls_ecp_group *grp,
                                           unsigned char p_eq_g )
{
    unsigned char w;

    /*
     * Minimize the number of multiplications, that is minimize
     * 10 * d * w + 18 * 2^(w-1) + 11 * d + 7 * w, with d = ceil( nbits / w )
     * (see costs of the various parts, with 1S = 1M)
     */
    w = grp->nbits >= 384 ? 5 : 4;

    /*
     * If P == G, pre-compute a bit more, since this may be re-used later.
     * Just adding one avoids upping the cost of the first mul too much,
     * and the memory cost too.
     */
    if( p_eq_g )
        w++;

    /*
     * Make sure w is within bounds.
     * (The last test is useful only for very small curves in the test suite.)
     */
#if( MBEDTLS_ECP_WINDOW_SIZE < 6 )
    if( w > MBEDTLS_ECP_WINDOW_SIZE )
        w = MBEDTLS_ECP_WINDOW_SIZE;
#endif
    if( w >= grp->nbits )
        w = 2;

    return( w );
}
static int ecp_normalize_mxz( const mbedtls_ecp_group *grp, mbedtls_ecp_point *P )
{
    int ret = ATBM_MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;

#if defined(MBEDTLS_ECP_NORMALIZE_MXZ_ALT)
    if( mbedtls_internal_ecp_grp_capable( grp ) )
        return( mbedtls_internal_ecp_normalize_mxz( grp, P ) );
#endif /* MBEDTLS_ECP_NORMALIZE_MXZ_ALT */

    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_inv_mod( &P->Z, &P->Z, &grp->P ) );
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_mul_mod( grp, &P->X, &P->X, &P->Z ) );
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_lset( &P->Z, 1 ) );

cleanup:
    return( ret );
}
static int ecp_double_add_mxz( const mbedtls_ecp_group *grp,
                               mbedtls_ecp_point *R, mbedtls_ecp_point *S,
                               const mbedtls_ecp_point *P, const mbedtls_ecp_point *Q,
                               const atbm_mbedtls_mpi *d )
{
    int ret = ATBM_MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;
    atbm_mbedtls_mpi A, AA, B, BB, E, C, D, DA, CB;

#if defined(MBEDTLS_ECP_DOUBLE_ADD_MXZ_ALT)
    if( mbedtls_internal_ecp_grp_capable( grp ) )
        return( mbedtls_internal_ecp_double_add_mxz( grp, R, S, P, Q, d ) );
#endif /* MBEDTLS_ECP_DOUBLE_ADD_MXZ_ALT */

    atbm_mbedtls_mpi_init( &A ); atbm_mbedtls_mpi_init( &AA ); atbm_mbedtls_mpi_init( &B );
    atbm_mbedtls_mpi_init( &BB ); atbm_mbedtls_mpi_init( &E ); atbm_mbedtls_mpi_init( &C );
    atbm_mbedtls_mpi_init( &D ); atbm_mbedtls_mpi_init( &DA ); atbm_mbedtls_mpi_init( &CB );

    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_add_mod( grp, &A,    &P->X,   &P->Z ) );
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_mul_mod( grp, &AA,   &A,      &A    ) );
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_sub_mod( grp, &B,    &P->X,   &P->Z ) );
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_mul_mod( grp, &BB,   &B,      &B    ) );
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_sub_mod( grp, &E,    &AA,     &BB   ) );
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_add_mod( grp, &C,    &Q->X,   &Q->Z ) );
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_sub_mod( grp, &D,    &Q->X,   &Q->Z ) );
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_mul_mod( grp, &DA,   &D,      &A    ) );
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_mul_mod( grp, &CB,   &C,      &B    ) );
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_add_mod( grp, &S->X, &DA,     &CB   ) );
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_mul_mod( grp, &S->X, &S->X,   &S->X ) );
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_sub_mod( grp, &S->Z, &DA,     &CB   ) );
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_mul_mod( grp, &S->Z, &S->Z,   &S->Z ) );
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_mul_mod( grp, &S->Z, d,       &S->Z ) );
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_mul_mod( grp, &R->X, &AA,     &BB   ) );
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_mul_mod( grp, &R->Z, &grp->A, &E    ) );
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_add_mod( grp, &R->Z, &BB,     &R->Z ) );
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_mul_mod( grp, &R->Z, &E,      &R->Z ) );

cleanup:
    atbm_mbedtls_mpi_free( &A ); atbm_mbedtls_mpi_free( &AA ); atbm_mbedtls_mpi_free( &B );
    atbm_mbedtls_mpi_free( &BB ); atbm_mbedtls_mpi_free( &E ); atbm_mbedtls_mpi_free( &C );
    atbm_mbedtls_mpi_free( &D ); atbm_mbedtls_mpi_free( &DA ); atbm_mbedtls_mpi_free( &CB );

    return( ret );
}
static int ecp_randomize_mxz( const mbedtls_ecp_group *grp, mbedtls_ecp_point *P,
                int (*f_rng)(void *, unsigned char *, size_t), void *p_rng )
{
    int ret = ATBM_MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;
    atbm_mbedtls_mpi l;
    size_t p_size;
    int count = 0;

#if defined(MBEDTLS_ECP_RANDOMIZE_MXZ_ALT)
    if( mbedtls_internal_ecp_grp_capable( grp ) )
        return( mbedtls_internal_ecp_randomize_mxz( grp, P, f_rng, p_rng );
#endif /* MBEDTLS_ECP_RANDOMIZE_MXZ_ALT */

    p_size = ( grp->pbits + 7 ) / 8;
    atbm_mbedtls_mpi_init( &l );

    /* Generate l such that 1 < l < p */
    do
    {
        MBEDTLS_MPI_CHK( atbm_ecp_mpi_fill_random( &l, p_size, f_rng, p_rng ) );

        while( atbm_mbedtls_mpi_cmp_mpi( &l, &grp->P ) >= 0 )
            MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_shift_r( &l, 1 ) );

        if( count++ > 10 )
        {
            ret = MBEDTLS_ERR_ECP_RANDOM_FAILED;
            goto cleanup;
        }
    }
    while( atbm_mbedtls_mpi_cmp_int( &l, 1 ) <= 0 );

    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_mul_mod( grp, &P->X, &P->X, &l ) );
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_mul_mod( grp, &P->Z, &P->Z, &l ) );

cleanup:
    atbm_mbedtls_mpi_free( &l );

    return( ret );
}
#define MOD_ADD( N )                                                    \
    while( atbm_mbedtls_mpi_cmp_mpi( &(N), &grp->P ) >= 0 )                  \
        MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_sub_abs( &(N), &(N), &grp->P ) )



static int mbedtls_ecp_mul_shortcuts( mbedtls_ecp_group *grp,
                                      mbedtls_ecp_point *R,
                                      const atbm_mbedtls_mpi *m,
                                      const mbedtls_ecp_point *P,
                                      void *rs_ctx )
{
    int ret = ATBM_MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;

    if( atbm_mbedtls_mpi_cmp_int( m, 1 ) == 0 )
    {
        MBEDTLS_MPI_CHK( mbedtls_ecp_copy( R, P ) );
    }
    else if( atbm_mbedtls_mpi_cmp_int( m, -1 ) == 0 )
    {
        MBEDTLS_MPI_CHK( mbedtls_ecp_copy( R, P ) );
        if( atbm_mbedtls_mpi_cmp_int( &R->Y, 0 ) != 0 )
            MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_sub_mpi( &R->Y, &grp->P, &R->Y ) );
    }
    else
    {
        MBEDTLS_MPI_CHK( mbedtls_ecp_mul_restartable( grp, R, m, P,
                                                      NULL, NULL, rs_ctx ) );
    }

cleanup:
    return( ret );
}
static int ecp_mul_comb( mbedtls_ecp_group *grp, mbedtls_ecp_point *R,
                         const atbm_mbedtls_mpi *m, const mbedtls_ecp_point *P,
                         int (*f_rng)(void *, unsigned char *, size_t),
                         void *p_rng,
                         void *rs_ctx )
{
    int ret = ATBM_MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;
    unsigned char w, p_eq_g, i;
    size_t d;
    unsigned char T_size = 0, T_ok = 0;
    mbedtls_ecp_point *T = NULL;
#if !defined(MBEDTLS_ECP_NO_INTERNAL_RNG)
#if defined(MBEDTLS_HMAC_DRBG_C) || defined(MBEDTLS_CTR_DRBG_C)
    atbm_ctr_drbg_context drbg_ctx;

    ecp_drbg_init( &drbg_ctx );
#endif /* MBEDTLS_HMAC_DRBG_C || MBEDTLS_CTR_DRBG_C */
#endif /* MBEDTLS_ECP_NO_INTERNAL_RNG */

    // ECP_RS_ENTER( rsm );

#if !defined(MBEDTLS_ECP_NO_INTERNAL_RNG)
#if defined(MBEDTLS_HMAC_DRBG_C) || defined(MBEDTLS_CTR_DRBG_C)
    if( f_rng == NULL )
    {
#if defined(MBEDTLS_ECP_RESTARTABLE)
        if( rs_ctx != NULL && rs_ctx->rsm != NULL )
            p_rng = &rs_ctx->rsm->drbg_ctx;
        else
#endif
            p_rng = &drbg_ctx;

        /* Initialize internal DRBG if necessary */
#if defined(MBEDTLS_ECP_RESTARTABLE)
        if( rs_ctx == NULL || rs_ctx->rsm == NULL ||
            rs_ctx->rsm->drbg_seeded == 0 )
#endif
        {
            const size_t m_len = ( grp->nbits + 7 ) / 8;
            MBEDTLS_MPI_CHK( ecp_drbg_seed( p_rng, m, m_len ) );
        }
#if defined(MBEDTLS_ECP_RESTARTABLE)
        if( rs_ctx != NULL && rs_ctx->rsm != NULL )
            rs_ctx->rsm->drbg_seeded = 1;
#endif
    }
#endif /* MBEDTLS_HMAC_DRBG_C || MBEDTLS_CTR_DRBG_C */
#endif /* MBEDTLS_ECP_NO_INTERNAL_RNG */

    /* Is P the base point ? */
#if MBEDTLS_ECP_FIXED_POINT_OPTIM == 1
    p_eq_g = ( atbm_mbedtls_mpi_cmp_mpi( &P->Y, &grp->G.Y ) == 0 &&
               atbm_mbedtls_mpi_cmp_mpi( &P->X, &grp->G.X ) == 0 );
#else
    p_eq_g = 0;
#endif

    /* Pick window size and deduce related sizes */
    w = ecp_pick_window_size( grp, p_eq_g );
    T_size = 1U << ( w - 1 );
    d = ( grp->nbits + w - 1 ) / w;

    /* Pre-computed table: do we have it already for the base point? */
    if( p_eq_g && grp->T != NULL )
    {
        /* second pointer to the same table, will be deleted on exit */
        T = grp->T;
        T_ok = 1;
    }
    else
#if defined(MBEDTLS_ECP_RESTARTABLE)
    /* Pre-computed table: do we have one in progress? complete? */
    if( rs_ctx != NULL && rs_ctx->rsm != NULL && rs_ctx->rsm->T != NULL )
    {
        /* transfer ownership of T from rsm to local function */
        T = rs_ctx->rsm->T;
        rs_ctx->rsm->T = NULL;
        rs_ctx->rsm->T_size = 0;

        /* This effectively jumps to the call to mul_comb_after_precomp() */
        T_ok = rs_ctx->rsm->state >= ecp_rsm_comb_core;
    }
    else
#endif
    /* Allocate table if we didn't have any */
    {
        T = mbedtls_calloc( T_size, sizeof( mbedtls_ecp_point ) );
        if( T == NULL )
        {
            ret = MBEDTLS_ERR_ECP_ALLOC_FAILED;
            goto cleanup;
        }

        for( i = 0; i < T_size; i++ )
            mbedtls_ecp_point_init( &T[i] );

        T_ok = 0;
    }

    /* Compute table (or finish computing it) if not done already */
    if( !T_ok )
    {
        MBEDTLS_MPI_CHK( ecp_precompute_comb( grp, T, P, w, d, rs_ctx ) );

        if( p_eq_g )
        {
            /* almost transfer ownership of T to the group, but keep a copy of
             * the pointer to use for calling the next function more easily */
            grp->T = T;
            grp->T_size = T_size;
        }
    }

    /* Actual comb multiplication using precomputed points */
    MBEDTLS_MPI_CHK( ecp_mul_comb_after_precomp( grp, R, m,
                                                 T, T_size, w, d,
                                                 f_rng, p_rng, rs_ctx ) );

cleanup:

#if !defined(MBEDTLS_ECP_NO_INTERNAL_RNG)
#if defined(MBEDTLS_HMAC_DRBG_C) || defined(MBEDTLS_CTR_DRBG_C)
    ecp_drbg_free( &drbg_ctx );
#endif /* MBEDTLS_HMAC_DRBG_C || MBEDTLS_CTR_DRBG_C */
#endif /* MBEDTLS_ECP_NO_INTERNAL_RNG */

    /* does T belong to the group? */
    if( T == grp->T )
        T = NULL;

    /* does T belong to the restart context? */
#if defined(MBEDTLS_ECP_RESTARTABLE)
    if( rs_ctx != NULL && rs_ctx->rsm != NULL && ret == MBEDTLS_ERR_ECP_IN_PROGRESS && T != NULL )
    {
        /* transfer ownership of T from local function to rsm */
        rs_ctx->rsm->T_size = T_size;
        rs_ctx->rsm->T = T;
        T = NULL;
    }
#endif

    /* did T belong to us? then let's destroy it! */
    if( T != NULL )
    {
        for( i = 0; i < T_size; i++ )
            mbedtls_ecp_point_free( &T[i] );
        mbedtls_free( T );
    }

    /* don't free R while in progress in case R == P */
#if defined(MBEDTLS_ECP_RESTARTABLE)
    if( ret != MBEDTLS_ERR_ECP_IN_PROGRESS )
#endif
    /* prevent caller from using invalid value */
    if( ret != 0 )
        mbedtls_ecp_point_free( R );

    // ECP_RS_LEAVE( rsm );

    return( ret );
}
/*
 * Multiplication with Montgomery ladder in x/z coordinates,
 * for curves in Montgomery form
 */
static int ecp_mul_mxz( mbedtls_ecp_group *grp, mbedtls_ecp_point *R,
                        const atbm_mbedtls_mpi *m, const mbedtls_ecp_point *P,
                        int (*f_rng)(void *, unsigned char *, size_t),
                        void *p_rng )
{
    int ret = ATBM_MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;
    size_t i;
    unsigned char b;
    mbedtls_ecp_point RP;
    atbm_mbedtls_mpi PX;
#if !defined(MBEDTLS_ECP_NO_INTERNAL_RNG)
#if defined(MBEDTLS_HMAC_DRBG_C) || defined(MBEDTLS_CTR_DRBG_C)
    atbm_ctr_drbg_context drbg_ctx;

    ecp_drbg_init( &drbg_ctx );
#endif /* MBEDTLS_HMAC_DRBG_C || MBEDTLS_CTR_DRBG_C */
#endif /* !MBEDTLS_ECP_NO_INTERNAL_RNG */
    mbedtls_ecp_point_init( &RP ); atbm_mbedtls_mpi_init( &PX );

#if !defined(MBEDTLS_ECP_NO_INTERNAL_RNG)
#if defined(MBEDTLS_HMAC_DRBG_C) || defined(MBEDTLS_CTR_DRBG_C)
    if( f_rng == NULL )
    {
        const size_t m_len = ( grp->nbits + 7 ) / 8;
        MBEDTLS_MPI_CHK( ecp_drbg_seed( &drbg_ctx, m, m_len ) );
        p_rng = &drbg_ctx;
    }
#endif /* MBEDTLS_HMAC_DRBG_C || MBEDTLS_CTR_DRBG_C */
#endif /* !MBEDTLS_ECP_NO_INTERNAL_RNG */

    /* Save PX and read from P before writing to R, in case P == R */
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_copy( &PX, &P->X ) );
    MBEDTLS_MPI_CHK( mbedtls_ecp_copy( &RP, P ) );

    /* Set R to zero in modified x/z coordinates */
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_lset( &R->X, 1 ) );
    MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_lset( &R->Z, 0 ) );
    atbm_mbedtls_mpi_free( &R->Y );

    /* RP.X might be sligtly larger than P, so reduce it */
    MOD_ADD( RP.X );

    /* Randomize coordinates of the starting point */
#if defined(MBEDTLS_ECP_NO_INTERNAL_RNG)
    if( f_rng != NULL )
#endif
        MBEDTLS_MPI_CHK( ecp_randomize_mxz( grp, &RP, f_rng, p_rng ) );

    /* Loop invariant: R = result so far, RP = R + P */
    i = atbm_mbedtls_mpi_bitlen( m ); /* one past the (zero-based) most significant bit */
    while( i-- > 0 )
    {
        b = atbm_mbedtls_mpi_get_bit( m, i );
        /*
         *  if (b) R = 2R + P else R = 2R,
         * which is:
         *  if (b) double_add( RP, R, RP, R )
         *  else   double_add( R, RP, R, RP )
         * but using safe conditional swaps to avoid leaks
         */
        MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_safe_cond_swap( &R->X, &RP.X, b ) );
        MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_safe_cond_swap( &R->Z, &RP.Z, b ) );
        MBEDTLS_MPI_CHK( ecp_double_add_mxz( grp, R, &RP, R, &RP, &PX ) );
        MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_safe_cond_swap( &R->X, &RP.X, b ) );
        MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_safe_cond_swap( &R->Z, &RP.Z, b ) );
    }

    /*
     * Knowledge of the projective coordinates may leak the last few bits of the
     * scalar [1], and since our MPI implementation isn't constant-flow,
     * inversion (used for coordinate normalization) may leak the full value
     * of its input via side-channels [2].
     *
     * [1] https://eprint.iacr.org/2003/191
     * [2] https://eprint.iacr.org/2020/055
     *
     * Avoid the leak by randomizing coordinates before we normalize them.
     */
#if defined(MBEDTLS_ECP_NO_INTERNAL_RNG)
    if( f_rng != NULL )
#endif
        MBEDTLS_MPI_CHK( ecp_randomize_mxz( grp, R, f_rng, p_rng ) );

    MBEDTLS_MPI_CHK( ecp_normalize_mxz( grp, R ) );

cleanup:
#if !defined(MBEDTLS_ECP_NO_INTERNAL_RNG)
#if defined(MBEDTLS_HMAC_DRBG_C) || defined(MBEDTLS_CTR_DRBG_C)
    ecp_drbg_free( &drbg_ctx );
#endif /* MBEDTLS_HMAC_DRBG_C || MBEDTLS_CTR_DRBG_C */
#endif /* !MBEDTLS_ECP_NO_INTERNAL_RNG */

    mbedtls_ecp_point_free( &RP ); atbm_mbedtls_mpi_free( &PX );

    return( ret );
}
/*
 * Get the type of a curve
 */
atbm_ecp_curve_type mbedtls_ecp_get_type( const mbedtls_ecp_group *grp )
{
    if( grp->G.X.p == NULL )
        return( ATBM_ECP_TYPE_NONE );

    if( grp->G.Y.p == NULL )
        return( ATBM_ECP_TYPE_MONTGOMERY );
    else
        return( ATBM_ECP_TYPE_SHORT_WEIERSTRASS );
}

/*
 * Generate a private key
 */
int mbedtls_ecp_gen_privkey( const mbedtls_ecp_group *grp,
                     atbm_mbedtls_mpi *d,
                     int (*f_rng)(void *, unsigned char *, size_t),
                     void *p_rng )
{
    int ret = MBEDTLS_ERR_ECP_BAD_INPUT_DATA;
    size_t n_size;

    ECP_VALIDATE_RET( grp   != NULL );
    ECP_VALIDATE_RET( d     != NULL );
    ECP_VALIDATE_RET( f_rng != NULL );

    n_size = ( grp->nbits + 7 ) / 8;

#if defined(MBEDTLS_ECP_MONTGOMERY_ENABLED)
    if( mbedtls_ecp_get_type( grp ) == ATBM_ECP_TYPE_MONTGOMERY )
    {
        /* [M225] page 5 */
        size_t b;

        do {
            MBEDTLS_MPI_CHK( atbm_ecp_mpi_fill_random( d, n_size, f_rng, p_rng ) );
        } while( atbm_mbedtls_mpi_bitlen( d ) == 0);

        /* Make sure the most significant bit is nbits */
        b = atbm_mbedtls_mpi_bitlen( d ) - 1; /* atbm_mbedtls_mpi_bitlen is one-based */
        if( b > grp->nbits )
            MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_shift_r( d, b - grp->nbits ) );
        else
            MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_set_bit( d, grp->nbits, 1 ) );

        /* Make sure the last two bits are unset for Curve448, three bits for
           Curve25519 */
        MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_set_bit( d, 0, 0 ) );
        MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_set_bit( d, 1, 0 ) );
        if( grp->nbits == 254 )
        {
            MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_set_bit( d, 2, 0 ) );
        }
    }
#endif /* MBEDTLS_ECP_MONTGOMERY_ENABLED */

#if defined(MBEDTLS_ECP_SHORT_WEIERSTRASS_ENABLED)
    if( mbedtls_ecp_get_type( grp ) == ATBM_ECP_TYPE_SHORT_WEIERSTRASS )
    {
        /* SEC1 3.2.1: Generate d such that 1 <= n < N */
        int count = 0;

        /*
         * Match the procedure given in RFC 6979 (deterministic ECDSA):
         * - use the same byte ordering;
         * - keep the leftmost nbits bits of the generated octet string;
         * - try until result is in the desired range.
         * This also avoids any biais, which is especially important for ECDSA.
         */
        do
        {
            MBEDTLS_MPI_CHK( atbm_ecp_mpi_fill_random( d, n_size, f_rng, p_rng ) );
            MBEDTLS_MPI_CHK( atbm_mbedtls_mpi_shift_r( d, 8 * n_size - grp->nbits ) );

            /*
             * Each try has at worst a probability 1/2 of failing (the msb has
             * a probability 1/2 of being 0, and then the result will be < N),
             * so after 30 tries failure probability is a most 2**(-30).
             *
             * For most curves, 1 try is enough with overwhelming probability,
             * since N starts with a lot of 1s in binary, but some curves
             * such as secp224k1 are actually very close to the worst case.
             */
            if( ++count > 30 )
            {
                ret = MBEDTLS_ERR_ECP_RANDOM_FAILED;
                goto cleanup;
            }

        }
        while( atbm_mbedtls_mpi_cmp_int( d, 1 ) < 0 ||
               atbm_mbedtls_mpi_cmp_mpi( d, &grp->N ) >= 0 );
    }
#endif /* MBEDTLS_ECP_SHORT_WEIERSTRASS_ENABLED */

cleanup:
    return( ret );
}

/*
 * Restartable multiplication R = m * P
 */
int mbedtls_ecp_mul_restartable( mbedtls_ecp_group *grp, mbedtls_ecp_point *R,
             const atbm_mbedtls_mpi *m, const mbedtls_ecp_point *P,
             int (*f_rng)(void *, unsigned char *, size_t), void *p_rng,
             void *rs_ctx )
{
    int ret = MBEDTLS_ERR_ECP_BAD_INPUT_DATA;
#if defined(MBEDTLS_ECP_INTERNAL_ALT)
    char is_grp_capable = 0;
#endif
    ECP_VALIDATE_RET( grp != NULL );
    ECP_VALIDATE_RET( R   != NULL );
    ECP_VALIDATE_RET( m   != NULL );
    ECP_VALIDATE_RET( P   != NULL );

#if defined(MBEDTLS_ECP_RESTARTABLE)
    /* reset ops count for this call if top-level */
    if( rs_ctx != NULL && rs_ctx->depth++ == 0 )
        rs_ctx->ops_done = 0;
#else
    (void) rs_ctx;
#endif

#if defined(MBEDTLS_ECP_INTERNAL_ALT)
    if( ( is_grp_capable = mbedtls_internal_ecp_grp_capable( grp ) ) )
        MBEDTLS_MPI_CHK( mbedtls_internal_ecp_init( grp ) );
#endif /* MBEDTLS_ECP_INTERNAL_ALT */

#if defined(MBEDTLS_ECP_RESTARTABLE)
    /* skip argument check when restarting */
    if( rs_ctx == NULL || rs_ctx->rsm == NULL )
#endif
    {
        /* Common sanity checks */
        MBEDTLS_MPI_CHK( mbedtls_ecp_check_privkey( grp, m ) );
        MBEDTLS_MPI_CHK( mbedtls_ecp_check_pubkey( grp, P ) );
    }

    ret = MBEDTLS_ERR_ECP_BAD_INPUT_DATA;
#if defined(MBEDTLS_ECP_MONTGOMERY_ENABLED)
    if( mbedtls_ecp_get_type( grp ) == ATBM_ECP_TYPE_MONTGOMERY )
        MBEDTLS_MPI_CHK( ecp_mul_mxz( grp, R, m, P, f_rng, p_rng ) );
#endif
#if defined(MBEDTLS_ECP_SHORT_WEIERSTRASS_ENABLED)
    if( mbedtls_ecp_get_type( grp ) == ATBM_ECP_TYPE_SHORT_WEIERSTRASS )
        MBEDTLS_MPI_CHK( ecp_mul_comb( grp, R, m, P, f_rng, p_rng, rs_ctx ) );
#endif

cleanup:

#if defined(MBEDTLS_ECP_INTERNAL_ALT)
    if( is_grp_capable )
        mbedtls_internal_ecp_free( grp );
#endif /* MBEDTLS_ECP_INTERNAL_ALT */

#if defined(MBEDTLS_ECP_RESTARTABLE)
    if( rs_ctx != NULL )
        rs_ctx->depth--;
#endif

    return( ret );
}

/*
 * Restartable linear combination
 * NOT constant-time
 */
int mbedtls_ecp_muladd_restartable(
             mbedtls_ecp_group *grp, mbedtls_ecp_point *R,
             const atbm_mbedtls_mpi *m, const mbedtls_ecp_point *P,
             const atbm_mbedtls_mpi *n, const mbedtls_ecp_point *Q,
             void *rs_ctx )
{
    int ret = ATBM_MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED;
    mbedtls_ecp_point mP;
    mbedtls_ecp_point *pmP = &mP;
    mbedtls_ecp_point *pR = R;
#if defined(MBEDTLS_ECP_INTERNAL_ALT)
    char is_grp_capable = 0;
#endif
    ECP_VALIDATE_RET( grp != NULL );
    ECP_VALIDATE_RET( R   != NULL );
    ECP_VALIDATE_RET( m   != NULL );
    ECP_VALIDATE_RET( P   != NULL );
    ECP_VALIDATE_RET( n   != NULL );
    ECP_VALIDATE_RET( Q   != NULL );

    if( mbedtls_ecp_get_type( grp ) != ATBM_ECP_TYPE_SHORT_WEIERSTRASS )
        return( MBEDTLS_ERR_ECP_FEATURE_UNAVAILABLE );

    mbedtls_ecp_point_init( &mP );

    // ECP_RS_ENTER( ma );

#if defined(MBEDTLS_ECP_RESTARTABLE)
    if( rs_ctx != NULL && rs_ctx->ma != NULL )
    {
        /* redirect intermediate results to restart context */
        pmP = &rs_ctx->ma->mP;
        pR  = &rs_ctx->ma->R;

        /* jump to next operation */
        if( rs_ctx->ma->state == ecp_rsma_mul2 )
            goto mul2;
        if( rs_ctx->ma->state == ecp_rsma_add )
            goto add;
        if( rs_ctx->ma->state == ecp_rsma_norm )
            goto norm;
    }
#endif /* MBEDTLS_ECP_RESTARTABLE */

    MBEDTLS_MPI_CHK( mbedtls_ecp_mul_shortcuts( grp, pmP, m, P, rs_ctx ) );
#if defined(MBEDTLS_ECP_RESTARTABLE)
    if( rs_ctx != NULL && rs_ctx->ma != NULL )
        rs_ctx->ma->state = ecp_rsma_mul2;

mul2:
#endif
    MBEDTLS_MPI_CHK( mbedtls_ecp_mul_shortcuts( grp, pR,  n, Q, rs_ctx ) );

#if defined(MBEDTLS_ECP_INTERNAL_ALT)
    if( ( is_grp_capable = mbedtls_internal_ecp_grp_capable( grp ) ) )
        MBEDTLS_MPI_CHK( mbedtls_internal_ecp_init( grp ) );
#endif /* MBEDTLS_ECP_INTERNAL_ALT */

#if defined(MBEDTLS_ECP_RESTARTABLE)
    if( rs_ctx != NULL && rs_ctx->ma != NULL )
        rs_ctx->ma->state = ecp_rsma_add;

add:
#endif
    MBEDTLS_MPI_CHK( ecp_add_mixed( grp, pR, pmP, pR ) );
#if defined(MBEDTLS_ECP_RESTARTABLE)
    if( rs_ctx != NULL && rs_ctx->ma != NULL )
        rs_ctx->ma->state = ecp_rsma_norm;

norm:
#endif
    MBEDTLS_MPI_CHK( ecp_normalize_jac( grp, pR ) );

#if defined(MBEDTLS_ECP_RESTARTABLE)
    if( rs_ctx != NULL && rs_ctx->ma != NULL )
        MBEDTLS_MPI_CHK( mbedtls_ecp_copy( R, pR ) );
#endif

cleanup:
#if defined(MBEDTLS_ECP_INTERNAL_ALT)
    if( is_grp_capable )
        mbedtls_internal_ecp_free( grp );
#endif /* MBEDTLS_ECP_INTERNAL_ALT */

    mbedtls_ecp_point_free( &mP );

    // ECP_RS_LEAVE( ma );

    return( ret );
}

#endif /* !MBEDTLS_ECP_ALT */
