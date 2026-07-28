#ifndef __crypto_wrapper_h__
#define __crypto_wrapper_h__

#ifdef __cplusplus
extern "C" {
#endif

#define DES_BLOCK_SIZE 8
#define AES_BLOCK_SIZE 16  //multiple of 16
#define AES_KEY_BITS   128 //128,192,256

int desEncryptInPlace(unsigned char *in, int inLen, /*out*/int *paddingLen, const unsigned char *key/*8*/);
//aes128
int aesEncryptInPlace(unsigned char *in, int inLen, /*out*/int *paddingLen, const unsigned char *iv/*16*/, const unsigned char *key/*16*/);

int desDecryptInPlace(unsigned char *in, int inLen, const unsigned char *key);
int desDecrypt(const unsigned char *in, int inLen, const unsigned char *key, unsigned char *out);

void sha1_md(const unsigned char *data, int len, unsigned char *digest/*20*/);
void sha1_hmac(const void *key, int key_len, const unsigned char *d, size_t n, unsigned char *md/*20*/);// unsigned int *md_len)
//bool ExtractP2pIdFromFile(const char *path, char p2pid[32], char initstr[256]);

void sha256_hmac(const unsigned char *key, int key_len, const unsigned char *data, int len, unsigned char digest[32]);
void sha256_md(const unsigned char *data, int len, unsigned char digest[32]);

void md5_md(const unsigned char *data, int len, unsigned char *digest/*16*/);
void md5_hmac(const void *key, int key_len, const unsigned char *data, size_t n, unsigned char *md/*16*/);

#ifdef __cplusplus
} /* extern "C" */
#endif


#endif /* end of include guard: __Decrypt_h__ */
