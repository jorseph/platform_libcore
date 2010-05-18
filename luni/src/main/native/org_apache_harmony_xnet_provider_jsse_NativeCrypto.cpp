/*
 * Copyright (C) 2007-2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * Native glue for Java class org.apache.harmony.xnet.provider.jsse.NativeCrypto
 */

#define LOG_TAG "NativeCrypto"

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <jni.h>

#include <JNIHelp.h>
#include <LocalArray.h>

#include <openssl/dsa.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>

#include "ScopedLocalRef.h"
#include "ScopedPrimitiveArray.h"
#include "ScopedUtfChars.h"
#include "UniquePtr.h"

#undef WITH_JNI_TRACE
#ifdef WITH_JNI_TRACE
#define JNI_TRACE(...) \
        ((void)LOG(LOG_INFO, LOG_TAG "-jni", __VA_ARGS__));     \
/*
        ((void)printf("I/" LOG_TAG "-jni:"));         \
        ((void)printf(__VA_ARGS__));          \
        ((void)printf("\n"))
*/
#else
#define JNI_TRACE(...) ((void)0)
#endif
/**
 * Frees the SSL error state.
 *
 * OpenSSL keeps an "error stack" per thread, and given that this code
 * can be called from arbitrary threads that we don't keep track of,
 * we err on the side of freeing the error state promptly (instead of,
 * say, at thread death).
 */
static void freeSslErrorState(void) {
    ERR_clear_error();
    ERR_remove_state(0);
}

/*
 * Checks this thread's OpenSSL error queue and throws a RuntimeException if
 * necessary.
 *
 * @return 1 if an exception was thrown, 0 if not.
 */
static int throwExceptionIfNecessary(JNIEnv* env, const char* /*location*/) {
    int error = ERR_get_error();
    int result = 0;

    if (error != 0) {
        char message[50];
        ERR_error_string_n(error, message, sizeof(message));
        // LOGD("OpenSSL error in %s %d: %s", location, error, message);
        jniThrowRuntimeException(env, message);
        result = 1;
    }

    freeSslErrorState();
    return result;
}


/**
 * Throws an SocketTimeoutException with the given string as a message.
 */
static void throwSocketTimeoutException(JNIEnv* env, const char* message) {
    if (jniThrowException(env, "java/net/SocketTimeoutException", message)) {
        LOGE("Unable to throw");
    }
}

/**
 * Throws a javax.net.ssl.SSLException with the given string as a message.
 */
static void throwSSLExceptionStr(JNIEnv* env, const char* message) {
    if (jniThrowException(env, "javax/net/ssl/SSLException", message)) {
        LOGE("Unable to throw");
    }
}

/**
 * Throws an SSLException with a message constructed from the current
 * SSL errors. This will also log the errors.
 *
 * @param env the JNI environment
 * @param sslErrorCode error code returned from SSL_get_error()
 * @param message null-ok; general error message
 */
static void throwSSLExceptionWithSslErrors(JNIEnv* env, SSL* ssl, int sslErrorCode, const char* message) {
    const char* messageStr = NULL;
    char* str;
    int ret;

    // First consult the SSL error code for the general message.
    switch (sslErrorCode) {
        case SSL_ERROR_NONE:
            messageStr = "Ok";
            break;
        case SSL_ERROR_SSL:
            messageStr = "Failure in SSL library, usually a protocol error";
            break;
        case SSL_ERROR_WANT_READ:
            messageStr = "SSL_ERROR_WANT_READ occured. You should never see this.";
            break;
        case SSL_ERROR_WANT_WRITE:
            messageStr = "SSL_ERROR_WANT_WRITE occured. You should never see this.";
            break;
        case SSL_ERROR_WANT_X509_LOOKUP:
            messageStr = "SSL_ERROR_WANT_X509_LOOKUP occured. You should never see this.";
            break;
        case SSL_ERROR_SYSCALL:
            messageStr = "I/O error during system call";
            break;
        case SSL_ERROR_ZERO_RETURN:
            messageStr = "SSL_ERROR_ZERO_RETURN occured. You should never see this.";
            break;
        case SSL_ERROR_WANT_CONNECT:
            messageStr = "SSL_ERROR_WANT_CONNECT occured. You should never see this.";
            break;
        case SSL_ERROR_WANT_ACCEPT:
            messageStr = "SSL_ERROR_WANT_ACCEPT occured. You should never see this.";
            break;
        default:
            messageStr = "Unknown SSL error";
    }

    // Prepend either our explicit message or a default one.
    if (asprintf(&str, "%s: ssl=%p: %s",
            (message != NULL) ? message : "SSL error", ssl, messageStr) <= 0) {
        // problem with asprintf
        throwSSLExceptionStr(env, messageStr);
        LOGV("%s", messageStr);
        freeSslErrorState();
        return;
    }

    char* allocStr = str;

    // For SSL protocol errors, SSL might have more information.
    if (sslErrorCode == SSL_ERROR_SSL) {
        // Append each error as an additional line to the message.
        for (;;) {
            char errStr[256];
            const char* file;
            int line;
            const char* data;
            int flags;
            unsigned long err =
                ERR_get_error_line_data(&file, &line, &data, &flags);
            if (err == 0) {
                break;
            }

            ERR_error_string_n(err, errStr, sizeof(errStr));

            ret = asprintf(&str, "%s\n%s (%s:%d %p:0x%08x)",
                    (allocStr == NULL) ? "" : allocStr,
                    errStr,
                    file,
                    line,
                    data,
                    flags);

            if (ret < 0) {
                break;
            }

            free(allocStr);
            allocStr = str;
        }
    // For errors during system calls, errno might be our friend.
    } else if (sslErrorCode == SSL_ERROR_SYSCALL) {
        if (asprintf(&str, "%s, %s", allocStr, strerror(errno)) >= 0) {
            free(allocStr);
            allocStr = str;
        }
    // If the error code is invalid, print it.
    } else if (sslErrorCode > SSL_ERROR_WANT_ACCEPT) {
        if (asprintf(&str, ", error code is %d", sslErrorCode) >= 0) {
            free(allocStr);
            allocStr = str;
        }
    }

    throwSSLExceptionStr(env, allocStr);

    LOGV("%s", allocStr);
    free(allocStr);
    freeSslErrorState();
}

/**
 * Helper function that grabs the casts an ssl pointer and then checks for nullness.
 * If this function returns NULL and <code>throwIfNull</code> is
 * passed as <code>true</code>, then this function will call
 * <code>throwSSLExceptionStr</code> before returning, so in this case of
 * NULL, a caller of this function should simply return and allow JNI
 * to do its thing.
 *
 * @param env the JNI environment
 * @param ssl_address; the ssl_address pointer as an integer
 * @param throwIfNull whether to throw if the SSL pointer is NULL
 * @returns the pointer, which may be NULL
 */
static SSL* to_SSL(JNIEnv* env, int ssl_address, bool throwIfNull) {
    SSL* ssl = reinterpret_cast<SSL*>(static_cast<uintptr_t>(ssl_address));
    if ((ssl == NULL) && throwIfNull) {
        throwSSLExceptionStr(env, "null SSL pointer");
    }

    return ssl;
}

static SSL_CTX* to_SSL_CTX(int ssl_ctx_address) {
    return reinterpret_cast<SSL_CTX*>(static_cast<uintptr_t>(ssl_ctx_address));
}

static SSL_SESSION* to_SSL_SESSION(int ssl_session_address) {
    return reinterpret_cast<SSL_SESSION*>(static_cast<uintptr_t>(ssl_session_address));
}

/**
 * Converts a Java byte[] to an OpenSSL BIGNUM, allocating the BIGNUM on the
 * fly.
 */
static BIGNUM* arrayToBignum(JNIEnv* env, jbyteArray source) {
    // LOGD("Entering arrayToBignum()");

    ScopedByteArray sourceBytes(env, source);
    return BN_bin2bn((unsigned char*) sourceBytes.get(), sourceBytes.size(), NULL);
}

/**
 * OpenSSL locking support. Taken from the O'Reilly book by Viega et al., but I
 * suppose there are not many other ways to do this on a Linux system (modulo
 * isomorphism).
 */
#define MUTEX_TYPE pthread_mutex_t
#define MUTEX_SETUP(x) pthread_mutex_init(&(x), NULL)
#define MUTEX_CLEANUP(x) pthread_mutex_destroy(&(x))
#define MUTEX_LOCK(x) pthread_mutex_lock(&(x))
#define MUTEX_UNLOCK(x) pthread_mutex_unlock(&(x))
#define THREAD_ID pthread_self()
#define THROW_EXCEPTION (-2)
#define THROW_SOCKETTIMEOUTEXCEPTION (-3)

static MUTEX_TYPE *mutex_buf = NULL;

static void locking_function(int mode, int n, const char*, int) {
    if (mode & CRYPTO_LOCK) {
        MUTEX_LOCK(mutex_buf[n]);
    } else {
        MUTEX_UNLOCK(mutex_buf[n]);
    }
}

static unsigned long id_function(void) {
    return ((unsigned long)THREAD_ID);
}

int THREAD_setup(void) {
    int i;

    mutex_buf = (MUTEX_TYPE *)malloc(CRYPTO_num_locks( ) * sizeof(MUTEX_TYPE));

    if(!mutex_buf) {
        return 0;
    }

    for (i = 0; i < CRYPTO_num_locks( ); i++) {
        MUTEX_SETUP(mutex_buf[i]);
    }

    CRYPTO_set_id_callback(id_function);
    CRYPTO_set_locking_callback(locking_function);

    return 1;
}

int THREAD_cleanup(void) {
    int i;

    if (!mutex_buf) {
      return 0;
    }

    CRYPTO_set_id_callback(NULL);
    CRYPTO_set_locking_callback(NULL);

    for (i = 0; i < CRYPTO_num_locks( ); i++) {
        MUTEX_CLEANUP(mutex_buf[i]);
    }

    free(mutex_buf);
    mutex_buf = NULL;

    return 1;
}

/**
 * Initialization phase for every OpenSSL job: Loads the Error strings, the
 * crypto algorithms and reset the OpenSSL library
 */
static void NativeCrypto_clinit(JNIEnv*, jclass)
{
    SSL_load_error_strings();
    ERR_load_crypto_strings();
    SSL_library_init();
    OpenSSL_add_all_algorithms();
    THREAD_setup();
}

/**
 * public static native int EVP_PKEY_new_DSA(byte[] p, byte[] q, byte[] g, byte[] pub_key, byte[] priv_key);
 */
static EVP_PKEY* NativeCrypto_EVP_PKEY_new_DSA(JNIEnv* env, jclass, jbyteArray p, jbyteArray q, jbyteArray g, jbyteArray pub_key, jbyteArray priv_key) {
    // LOGD("Entering EVP_PKEY_new_DSA()");

    DSA* dsa = DSA_new();
    if (dsa == NULL) {
        jniThrowRuntimeException(env, "DSA_new failed");
        return NULL;
    }

    dsa->p = arrayToBignum(env, p);
    dsa->q = arrayToBignum(env, q);
    dsa->g = arrayToBignum(env, g);
    dsa->pub_key = arrayToBignum(env, pub_key);

    if (priv_key != NULL) {
        dsa->priv_key = arrayToBignum(env, priv_key);
    }

    if (dsa->p == NULL || dsa->q == NULL || dsa->g == NULL || dsa->pub_key == NULL) {
        DSA_free(dsa);
        jniThrowRuntimeException(env, "Unable to convert BigInteger to BIGNUM");
        return NULL;
    }

    EVP_PKEY* pkey = EVP_PKEY_new();
    if (pkey == NULL) {
        jniThrowRuntimeException(env, "EVP_PKEY_new failed");
        return NULL;
    }
    EVP_PKEY_assign_DSA(pkey, dsa);

    return pkey;
}

/**
 * private static native int EVP_PKEY_new_RSA(byte[] n, byte[] e, byte[] d, byte[] p, byte[] q);
 */
static EVP_PKEY* NativeCrypto_EVP_PKEY_new_RSA(JNIEnv* env, jclass, jbyteArray n, jbyteArray e, jbyteArray d, jbyteArray p, jbyteArray q) {
    // LOGD("Entering EVP_PKEY_new_RSA()");

    RSA* rsa = RSA_new();
    if (rsa == NULL) {
        jniThrowRuntimeException(env, "RSA_new failed");
        return NULL;
    }

    rsa->n = arrayToBignum(env, n);
    rsa->e = arrayToBignum(env, e);

    if (d != NULL) {
        rsa->d = arrayToBignum(env, d);
    }

    if (p != NULL) {
        rsa->p = arrayToBignum(env, p);
    }

    if (q != NULL) {
        rsa->q = arrayToBignum(env, q);
    }

    // int check = RSA_check_key(rsa);
    // LOGI("RSA_check_key returns %d", check);

    if (rsa->n == NULL || rsa->e == NULL) {
        RSA_free(rsa);
        jniThrowRuntimeException(env, "Unable to convert BigInteger to BIGNUM");
        return NULL;
    }

    EVP_PKEY* pkey = EVP_PKEY_new();
    if (pkey == NULL) {
        RSA_free(rsa);
        jniThrowRuntimeException(env, "EVP_PKEY_new failed");
        return NULL;
    }
    EVP_PKEY_assign_RSA(pkey, rsa);

    return pkey;
}

/**
 * private static native void EVP_PKEY_free(int pkey);
 */
static void NativeCrypto_EVP_PKEY_free(JNIEnv*, jclass, EVP_PKEY* pkey) {
    // LOGD("Entering EVP_PKEY_free()");

    if (pkey != NULL) {
        EVP_PKEY_free(pkey);
    }
}

/*
 * public static native int EVP_new()
 */
static jint NativeCrypto_EVP_new(JNIEnv*, jclass) {
    // LOGI("NativeCrypto_EVP_DigestNew");

    return (jint)EVP_MD_CTX_create();
}

/*
 * public static native void EVP_free(int)
 */
static void NativeCrypto_EVP_free(JNIEnv*, jclass, EVP_MD_CTX* ctx) {
    // LOGI("NativeCrypto_EVP_DigestFree");

    if (ctx != NULL) {
        EVP_MD_CTX_destroy(ctx);
    }
}

/*
 * public static native int EVP_DigestFinal(int, byte[], int)
 */
static jint NativeCrypto_EVP_DigestFinal(JNIEnv* env, jclass, EVP_MD_CTX* ctx, jbyteArray hash, jint offset) {
    // LOGI("NativeCrypto_EVP_DigestFinal%x, %x, %d, %d", ctx, hash, offset);

    if (ctx == NULL || hash == NULL) {
        jniThrowNullPointerException(env, NULL);
        return -1;
    }

    int result = -1;

    jbyte* hashBytes = env->GetByteArrayElements(hash, NULL);
    EVP_DigestFinal(ctx, (unsigned char*) (hashBytes + offset), (unsigned int*)&result);
    env->ReleaseByteArrayElements(hash, hashBytes, 0);

    throwExceptionIfNecessary(env, "NativeCrypto_EVP_DigestFinal");

    return result;
}

/*
 * public static native void EVP_DigestInit(int, java.lang.String)
 */
static void NativeCrypto_EVP_DigestInit(JNIEnv* env, jclass, EVP_MD_CTX* ctx, jstring algorithm) {
    // LOGI("NativeCrypto_EVP_DigestInit");

    if (ctx == NULL || algorithm == NULL) {
        jniThrowNullPointerException(env, NULL);
        return;
    }

    ScopedUtfChars algorithmChars(env, algorithm);

    const EVP_MD *digest = EVP_get_digestbynid(OBJ_txt2nid(algorithmChars.c_str()));

    if (digest == NULL) {
        jniThrowRuntimeException(env, "Hash algorithm not found");
        return;
    }

    EVP_DigestInit(ctx, digest);

    throwExceptionIfNecessary(env, "NativeCrypto_EVP_DigestInit");
}

/*
 * public static native void EVP_DigestSize(int)
 */
static jint NativeCrypto_EVP_DigestSize(JNIEnv* env, jclass, EVP_MD_CTX* ctx) {
    // LOGI("NativeCrypto_EVP_DigestSize");

    if (ctx == NULL) {
        jniThrowNullPointerException(env, NULL);
        return -1;
    }

    int result = EVP_MD_CTX_size(ctx);

    throwExceptionIfNecessary(env, "NativeCrypto_EVP_DigestSize");

    return result;
}

/*
 * public static native void EVP_DigestBlockSize(int)
 */
static jint NativeCrypto_EVP_DigestBlockSize(JNIEnv* env, jclass, EVP_MD_CTX* ctx) {
    // LOGI("NativeCrypto_EVP_DigestBlockSize");

    if (ctx == NULL) {
        jniThrowNullPointerException(env, NULL);
        return -1;
    }

    int result = EVP_MD_CTX_block_size(ctx);

    throwExceptionIfNecessary(env, "NativeCrypto_EVP_DigestBlockSize");

    return result;
}

/*
 * public static native void EVP_DigestUpdate(int, byte[], int, int)
 */
static void NativeCrypto_EVP_DigestUpdate(JNIEnv* env, jclass, EVP_MD_CTX* ctx, jbyteArray buffer, jint offset, jint length) {
    // LOGI("NativeCrypto_EVP_DigestUpdate %x, %x, %d, %d", ctx, buffer, offset, length);

    if (ctx == NULL || buffer == NULL) {
        jniThrowNullPointerException(env, NULL);
        return;
    }

    ScopedByteArray bufferBytes(env, buffer);
    EVP_DigestUpdate(ctx, (unsigned char*) (bufferBytes.get() + offset), length);

    throwExceptionIfNecessary(env, "NativeCrypto_EVP_DigestUpdate");
}

/*
 * public static native void EVP_VerifyInit(int, java.lang.String)
 */
static void NativeCrypto_EVP_VerifyInit(JNIEnv* env, jclass, EVP_MD_CTX* ctx, jstring algorithm) {
    // LOGI("NativeCrypto_EVP_VerifyInit");

    if (ctx == NULL || algorithm == NULL) {
        jniThrowNullPointerException(env, NULL);
        return;
    }

    ScopedUtfChars algorithmChars(env, algorithm);

    const EVP_MD *digest = EVP_get_digestbynid(OBJ_txt2nid(algorithmChars.c_str()));

    if (digest == NULL) {
        jniThrowRuntimeException(env, "Hash algorithm not found");
        return;
    }

    EVP_VerifyInit(ctx, digest);

    throwExceptionIfNecessary(env, "NativeCrypto_EVP_VerifyInit");
}

/*
 * public static native void EVP_VerifyUpdate(int, byte[], int, int)
 */
static void NativeCrypto_EVP_VerifyUpdate(JNIEnv* env, jclass, EVP_MD_CTX* ctx, jbyteArray buffer, jint offset, jint length) {
    // LOGI("NativeCrypto_EVP_VerifyUpdate %x, %x, %d, %d", ctx, buffer, offset, length);

    if (ctx == NULL || buffer == NULL) {
        jniThrowNullPointerException(env, NULL);
        return;
    }

    ScopedByteArray bufferBytes(env, buffer);
    EVP_VerifyUpdate(ctx, (unsigned char*) (bufferBytes.get() + offset), length);

    throwExceptionIfNecessary(env, "NativeCrypto_EVP_VerifyUpdate");
}

/*
 * public static native void EVP_VerifyFinal(int, byte[], int, int, int)
 */
static int NativeCrypto_EVP_VerifyFinal(JNIEnv* env, jclass, EVP_MD_CTX* ctx, jbyteArray buffer, jint offset, jint length, EVP_PKEY* pkey) {
    // LOGI("NativeCrypto_EVP_VerifyFinal %x, %x, %d, %d %x", ctx, buffer, offset, length, pkey);

    if (ctx == NULL || buffer == NULL || pkey == NULL) {
        jniThrowNullPointerException(env, NULL);
        return -1;
    }

    ScopedByteArray bufferBytes(env, buffer);
    int result = EVP_VerifyFinal(ctx, (unsigned char*) (bufferBytes.get() + offset), length, pkey);

    throwExceptionIfNecessary(env, "NativeCrypto_EVP_VerifyFinal");

    return result;
}

/**
 * Helper function that creates an RSA public key from two buffers containing
 * the big-endian bit representation of the modulus and the public exponent.
 *
 * @param mod The data of the modulus
 * @param modLen The length of the modulus data
 * @param exp The data of the exponent
 * @param expLen The length of the exponent data
 *
 * @return A pointer to the new RSA structure, or NULL on error
 */
static RSA* rsaCreateKey(unsigned char* mod, int modLen, unsigned char* exp, int expLen) {
    // LOGD("Entering rsaCreateKey()");

    RSA* rsa = RSA_new();
    if (rsa == NULL) {
        return NULL;
    }

    rsa->n = BN_bin2bn(mod, modLen, NULL);
    rsa->e = BN_bin2bn(exp, expLen, NULL);

    if (rsa->n == NULL || rsa->e == NULL) {
        RSA_free(rsa);
        return NULL;
    }

    return rsa;
}

/**
 * Helper function that verifies a given RSA signature for a given message.
 *
 * @param msg The message to verify
 * @param msgLen The length of the message
 * @param sig The signature to verify
 * @param sigLen The length of the signature
 * @param algorithm The name of the hash/sign algorithm to use, e.g. "RSA-SHA1"
 * @param rsa The RSA public key to use
 *
 * @return 1 on success, 0 on failure, -1 on error (check SSL errors then)
 *
 */
static int rsaVerify(unsigned char* msg, unsigned int msgLen, unsigned char* sig,
                     unsigned int sigLen, char* algorithm, RSA* rsa) {

    // LOGD("Entering rsaVerify(%x, %d, %x, %d, %s, %x)", msg, msgLen, sig, sigLen, algorithm, rsa);

    EVP_PKEY* pkey = EVP_PKEY_new();
    if (pkey == NULL) {
        return -1;
    }
    EVP_PKEY_set1_RSA(pkey, rsa);

    const EVP_MD *type = EVP_get_digestbyname(algorithm);
    if (type == NULL) {
        EVP_PKEY_free(pkey);
        return -1;
    }

    EVP_MD_CTX ctx;
    EVP_MD_CTX_init(&ctx);
    if (EVP_VerifyInit_ex(&ctx, type, NULL) == 0) {
        EVP_PKEY_free(pkey);
        return -1;
    }

    EVP_VerifyUpdate(&ctx, msg, msgLen);
    int result = EVP_VerifyFinal(&ctx, sig, sigLen, pkey);
    EVP_MD_CTX_cleanup(&ctx);
    EVP_PKEY_free(pkey);
    return result;
}

/**
 * Verifies an RSA signature.
 */
static int NativeCrypto_verifysignature(JNIEnv* env, jclass,
        jbyteArray msg, jbyteArray sig, jstring algorithm, jbyteArray mod, jbyteArray exp) {

    JNI_TRACE("NativeCrypto_verifysignature msg=%p sig=%p algorithm=%p mod=%p exp%p",
             msg, sig, algorithm, mod, exp);

    if (msg == NULL || sig == NULL || algorithm == NULL || mod == NULL || exp == NULL) {
        jniThrowNullPointerException(env, NULL);
        JNI_TRACE("NativeCrypto_verifysignature => -1");
        return -1;
    }

    int result = -1;

    ScopedByteArray msgBytes(env, msg);
    ScopedByteArray sigBytes(env, sig);
    ScopedByteArray modBytes(env, mod);
    ScopedByteArray expBytes(env, exp);

    ScopedUtfChars algorithmChars(env, algorithm);
    JNI_TRACE("NativeCrypto_verifysignature algorithmChars=%s", algorithmChars.c_str());

    RSA* rsa = rsaCreateKey((unsigned char*) modBytes.get(), modBytes.size(), (unsigned char*) expBytes.get(), expBytes.size());
    if (rsa != NULL) {
        result = rsaVerify((unsigned char*) msgBytes.get(), msgBytes.size(), (unsigned char*) sigBytes.get(), sigBytes.size(),
                (char*) algorithmChars.c_str(), rsa);
        RSA_free(rsa);
    }

    if (result == -1) {
        if (!throwExceptionIfNecessary(env, "NativeCrypto_verifysignature")) {
            jniThrowRuntimeException(env, "Internal error during verification");
        }
    }

    JNI_TRACE("NativeCrypto_verifysignature => %d", result);
    return result;
}

/**
 * Convert ssl version constant to string. Based on SSL_get_version
 */
// TODO move to jsse.patch
static const char* get_ssl_version(int ssl_version) {
    switch (ssl_version) {
        // newest to oldest
        case TLS1_VERSION: {
          return SSL_TXT_TLSV1;
        }
        case SSL3_VERSION: {
          return SSL_TXT_SSLV3;
        }
        case SSL2_VERSION: {
          return SSL_TXT_SSLV2;
        }
        default: {
          return "unknown";
        }
    }
}

#ifdef WITH_JNI_TRACE
/**
 * Convert content type constant to string.
 */
// TODO move to jsse.patch
static const char* get_content_type(int content_type) {
    switch (content_type) {
        case SSL3_RT_CHANGE_CIPHER_SPEC: {
            return "SSL3_RT_CHANGE_CIPHER_SPEC";
        }
        case SSL3_RT_ALERT: {
            return "SSL3_RT_ALERT";
        }
        case SSL3_RT_HANDSHAKE: {
            return "SSL3_RT_HANDSHAKE";
        }
        case SSL3_RT_APPLICATION_DATA: {
            return "SSL3_RT_APPLICATION_DATA";
        }
        default: {
            LOGD("Unknown TLS/SSL content type %d", content_type);
            return "<unknown>";
        }
    }
}
#endif

#ifdef WITH_JNI_TRACE
/**
 * Simple logging call back to show hand shake messages
 */
static void ssl_msg_callback_LOG(int write_p, int ssl_version, int content_type,
                                 const void *buf, size_t len, SSL* ssl, void* arg) {
  JNI_TRACE("ssl=%p SSL msg %s %s %s %p %d %p",
           ssl,
           (write_p) ? "send" : "recv",
           get_ssl_version(ssl_version),
           get_content_type(content_type),
           buf,
           len,
           arg);
}
#endif

#ifdef WITH_JNI_TRACE
/**
 * Based on example logging call back from SSL_CTX_set_info_callback man page
 */
static void info_callback_LOG(const SSL* s __attribute__ ((unused)), int where, int ret)
{
    int w = where & ~SSL_ST_MASK;
    const char* str;
    if (w & SSL_ST_CONNECT) {
        str = "SSL_connect";
    } else if (w & SSL_ST_ACCEPT) {
        str = "SSL_accept";
    } else {
        str = "undefined";
    }

    if (where & SSL_CB_LOOP) {
        JNI_TRACE("ssl=%p %s:%s %s", s, str, SSL_state_string(s), SSL_state_string_long(s));
    } else if (where & SSL_CB_ALERT) {
        str = (where & SSL_CB_READ) ? "read" : "write";
        JNI_TRACE("ssl=%p SSL3 alert %s:%s:%s %s %s",
                  s,
                  str,
                  SSL_alert_type_string(ret),
                  SSL_alert_desc_string(ret),
                  SSL_alert_type_string_long(ret),
                  SSL_alert_desc_string_long(ret));
    } else if (where & SSL_CB_EXIT) {
        if (ret == 0) {
            JNI_TRACE("ssl=%p %s:failed exit in %s %s",
                      s, str, SSL_state_string(s), SSL_state_string_long(s));
        } else if (ret < 0) {
            JNI_TRACE("ssl=%p %s:error exit in %s %s",
                      s, str, SSL_state_string(s), SSL_state_string_long(s));
        } else if (ret == 1) {
            JNI_TRACE("ssl=%p %s:ok exit in %s %s",
                      s, str, SSL_state_string(s), SSL_state_string_long(s));
        } else {
            JNI_TRACE("ssl=%p %s:unknown exit %d in %s %s",
                      s, str, ret, SSL_state_string(s), SSL_state_string_long(s));
        }
    } else if (where & SSL_CB_HANDSHAKE_START) {
        JNI_TRACE("ssl=%p handshake start in %s %s",
                  s, SSL_state_string(s), SSL_state_string_long(s));
    } else if (where & SSL_CB_HANDSHAKE_DONE) {
        JNI_TRACE("ssl=%p handshake done in %s %s",
                  s, SSL_state_string(s), SSL_state_string_long(s));
    } else {
        JNI_TRACE("ssl=%p %s:unknown where %d in %s %s",
                  s, str, where, SSL_state_string(s), SSL_state_string_long(s));
    }
}
#endif

/**
 * Returns an array containing all the X509 certificate's bytes.
 */
static jobjectArray getCertificateBytes(JNIEnv* env,
        const STACK_OF(X509) *chain)
{
    if (chain == NULL) {
        // Chain can be NULL if the associated cipher doesn't do certs.
        return NULL;
    }

    int count = sk_X509_num(chain);
    if (count <= 0) {
        return NULL;
    }

    jobjectArray joa = env->NewObjectArray(count, env->FindClass("[B"), NULL);
    if (joa == NULL) {
        return NULL;
    }

    BIO* bio = BIO_new(BIO_s_mem());
    if (bio == NULL) {
        jniThrowRuntimeException(env, "BIO_new failed");
        return NULL;
    }

    // LOGD("Start fetching the certificates");
    for (int i = 0; i < count; i++) {
        X509* cert = sk_X509_value(chain, i);

        BIO_reset(bio);
        PEM_write_bio_X509(bio, cert);

        BUF_MEM* bptr;
        BIO_get_mem_ptr(bio, &bptr);
        jbyteArray bytes = env->NewByteArray(bptr->length);

        if (bytes == NULL) {
            /*
             * Indicate an error by resetting joa to NULL. It will
             * eventually get gc'ed.
             */
            joa = NULL;
            break;
        }
        jbyte* src = reinterpret_cast<jbyte*>(bptr->data);
        env->SetByteArrayRegion(bytes, 0, bptr->length, src);
        env->SetObjectArrayElement(joa, i, bytes);
    }

    // LOGD("Certificate fetching complete");
    BIO_free(bio);
    return joa;
}

/**
 * Our additional application data needed for getting synchronization right.
 * This maybe warrants a bit of lengthy prose:
 *
 * (1) We use a flag to reflect whether we consider the SSL connection alive.
 * Any read or write attempt loops will be cancelled once this flag becomes 0.
 *
 * (2) We use an int to count the number of threads that are blocked by the
 * underlying socket. This may be at most two (one reader and one writer), since
 * the Java layer ensures that no more threads will enter the native code at the
 * same time.
 *
 * (3) The pipe is used primarily as a means of cancelling a blocking select()
 * when we want to close the connection (aka "emergency button"). It is also
 * necessary for dealing with a possible race condition situation: There might
 * be cases where both threads see an SSL_ERROR_WANT_READ or
 * SSL_ERROR_WANT_WRITE. Both will enter a select() with the proper argument.
 * If one leaves the select() successfully before the other enters it, the
 * "success" event is already consumed and the second thread will be blocked,
 * possibly forever (depending on network conditions).
 *
 * The idea for solving the problem looks like this: Whenever a thread is
 * successful in moving around data on the network, and it knows there is
 * another thread stuck in a select(), it will write a byte to the pipe, waking
 * up the other thread. A thread that returned from select(), on the other hand,
 * knows whether it's been woken up by the pipe. If so, it will consume the
 * byte, and the original state of affairs has been restored.
 *
 * The pipe may seem like a bit of overhead, but it fits in nicely with the
 * other file descriptors of the select(), so there's only one condition to wait
 * for.
 *
 * (4) Finally, a mutex is needed to make sure that at most one thread is in
 * either SSL_read() or SSL_write() at any given time. This is an OpenSSL
 * requirement. We use the same mutex to guard the field for counting the
 * waiting threads.
 *
 * Note: The current implementation assumes that we don't have to deal with
 * problems induced by multiple cores or processors and their respective
 * memory caches. One possible problem is that of inconsistent views on the
 * "aliveAndKicking" field. This could be worked around by also enclosing all
 * accesses to that field inside a lock/unlock sequence of our mutex, but
 * currently this seems a bit like overkill. Marking volatile at the very least.
 *
 * During handshaking, three additional fields are used to up-call into
 * Java to perform certificate verification and handshake completion.
 *
 * (5) the JNIEnv so we can invoke the Java callback
 *
 * (6) a NativeCrypto.CertificateChainVerifier to call with the peer certificate chain
 *
 * (7) a NativeCrypto.HandshakeCompletedCallback to call back when handshake is done
 *
 * These fields are cleared by the info_callback the handshake has
 * completed. SSL_VERIFY_CLIENT_ONCE is currently used to disable
 * renegotiation but if that changes, care would need to be taken to
 * maintain an appropriate JNIEnv on any downcall to openssl that
 * could result in an upcall to Java. The current code does try to
 * cover these cases by conditionally setting the JNIenv on calls that
 * can read and write to the SSL such as SSL_do_handshake, SSL_read,
 * SSL_write, and SSL_shutdown if handshaking is not complete.
 *
 */
class AppData {
  public:
    volatile int aliveAndKicking;
    int waitingThreads;
    int fdsEmergency[2];
    MUTEX_TYPE mutex;
    JNIEnv* env;
    jobject certificateChainVerifier;
    jobject handshakeCompletedCallback;

    /**
     * Creates our application data and attaches it to a given SSL connection.
     *
     * @param env The JNIEnv
     * @param ccv The CertificateChainVerifier
     * @param hcc The HandshakeCompletedCallback
     */
  public:
    static AppData* create(JNIEnv* env,
                           jobject ccv,
                           jobject hcc) {
        if (ccv == NULL || hcc == NULL) {
            return NULL;
        }
        AppData* appData = new AppData(env);
        if (pipe(appData->fdsEmergency) == -1) {
            destroy(env, appData);
            return NULL;
        }
        if (MUTEX_SETUP(appData->mutex) == -1) {
            destroy(env, appData);
            return NULL;
        }
        appData->certificateChainVerifier = env->NewGlobalRef(ccv);
        if (appData->certificateChainVerifier == NULL) {
            destroy(env, appData);
            return NULL;
        }
        appData->handshakeCompletedCallback = env->NewGlobalRef(hcc);
        if (appData->handshakeCompletedCallback == NULL) {
            destroy(env, appData);
            return NULL;
        }
        return appData;
    }

    static void destroy(JNIEnv* env, AppData* appData) {
        if (appData == NULL) {
            return;
        }
        appData->cleanupGlobalRefs(env);
        delete appData;
    }

  private:
    AppData(JNIEnv* env) :
            aliveAndKicking(1),
            waitingThreads(0),
            env(NULL),
            certificateChainVerifier(NULL),
            handshakeCompletedCallback(NULL) {
        setEnv(env);
        fdsEmergency[0] = -1;
        fdsEmergency[1] = -1;
    }

    /**
     * Destroys our application data, cleaning up everything in the process.
     */
    ~AppData() {
        aliveAndKicking = 0;
        if (fdsEmergency[0] != -1) {
            close(fdsEmergency[0]);
        }
        if (fdsEmergency[1] != -1) {
            close(fdsEmergency[1]);
        }
        MUTEX_CLEANUP(mutex);
    }

    void cleanupGlobalRefs(JNIEnv* env) {
        if (certificateChainVerifier != NULL) {
            env->DeleteGlobalRef(certificateChainVerifier);
            certificateChainVerifier = NULL;
        }
        if (handshakeCompletedCallback != NULL) {
            env->DeleteGlobalRef(handshakeCompletedCallback);
            handshakeCompletedCallback = NULL;
        }
        clearEnv();
    }

  public:
    void setEnv(JNIEnv* e) {
        env = e;
    }

    void clearEnv() {
        env = NULL;
    }

    void handshakeCompleted(JNIEnv* e) {
        cleanupGlobalRefs(e);
    }
};

/**
 * Dark magic helper function that checks, for a given SSL session, whether it
 * can SSL_read() or SSL_write() without blocking. Takes into account any
 * concurrent attempts to close the SSL session from the Java side. This is
 * needed to get rid of the hangs that occur when thread #1 closes the SSLSocket
 * while thread #2 is sitting in a blocking read or write. The type argument
 * specifies whether we are waiting for readability or writability. It expects
 * to be passed either SSL_ERROR_WANT_READ or SSL_ERROR_WANT_WRITE, since we
 * only need to wait in case one of these problems occurs.
 *
 * @param type Either SSL_ERROR_WANT_READ or SSL_ERROR_WANT_WRITE
 * @param fd The file descriptor to wait for (the underlying socket)
 * @param data The application data structure with mutex info etc.
 * @param timeout The timeout value for select call, with the special value
 *                0 meaning no timeout at all (wait indefinitely). Note: This is
 *                the Java semantics of the timeout value, not the usual
 *                select() semantics.
 * @return The result of the inner select() call, -1 on additional errors
 */
static int sslSelect(int type, int fd, AppData* appData, int timeout) {
    fd_set rfds;
    fd_set wfds;

    FD_ZERO(&rfds);
    FD_ZERO(&wfds);

    if (type == SSL_ERROR_WANT_READ) {
        FD_SET(fd, &rfds);
    } else {
        FD_SET(fd, &wfds);
    }

    FD_SET(appData->fdsEmergency[0], &rfds);

    int max = fd > appData->fdsEmergency[0] ? fd : appData->fdsEmergency[0];

    // Build a struct for the timeout data if we actually want a timeout.
    struct timeval tv;
    struct timeval *ptv;
    if (timeout > 0) {
        tv.tv_sec = timeout / 1000;
        tv.tv_usec = 0;
        ptv = &tv;
    } else {
        ptv = NULL;
    }

    // LOGD("Doing select() for SSL_ERROR_WANT_%s...", type == SSL_ERROR_WANT_READ ? "READ" : "WRITE");
    int result = select(max + 1, &rfds, &wfds, NULL, ptv);
    // LOGD("Returned from select(), result is %d", result);

    // Lock
    if (MUTEX_LOCK(appData->mutex) == -1) {
        return -1;
    }

    // If we have been woken up by the emergency pipe, there must be a token in
    // it. Thus we can safely read it (even in a blocking way).
    if (FD_ISSET(appData->fdsEmergency[0], &rfds)) {
        char token;
        do {
            read(appData->fdsEmergency[0], &token, 1);
        } while (errno == EINTR);
    }

    // Tell the world that there is now one thread less waiting for the
    // underlying network.
    appData->waitingThreads--;

    // Unlock
    MUTEX_UNLOCK(appData->mutex);
    // LOGD("leave sslSelect");
    return result;
}

/**
 * Helper function that wakes up a thread blocked in select(), in case there is
 * one. Is being called by sslRead() and sslWrite() as well as by JNI glue
 * before closing the connection.
 *
 * @param data The application data structure with mutex info etc.
 */
static void sslNotify(AppData* appData) {
    // Write a byte to the emergency pipe, so a concurrent select() can return.
    // Note we have to restore the errno of the original system call, since the
    // caller relies on it for generating error messages.
    int errnoBackup = errno;
    char token = '*';
    do {
        errno = 0;
        write(appData->fdsEmergency[1], &token, 1);
    } while (errno == EINTR);
    errno = errnoBackup;
}

// From private header file external/openssl/ssl_locl.h
// TODO move dependant code to jsse.patch to avoid dependency
#define SSL_aRSA                0x00000001L
#define SSL_aDSS                0x00000002L
#define SSL_aNULL               0x00000004L
#define SSL_aDH                 0x00000008L
#define SSL_aECDH               0x00000010L
#define SSL_aKRB5               0x00000020L
#define SSL_aECDSA              0x00000040L
#define SSL_aPSK                0x00000080L

/**
 * Converts an SSL_CIPHER's algorithms field to a TrustManager auth argument
 */
// TODO move to jsse.patch
static const char* SSL_CIPHER_authentication_method(const SSL_CIPHER* cipher)
{
    unsigned long alg_auth = cipher->algorithm_auth;

    const char *au;
    switch (alg_auth) {
        case SSL_aRSA:
            au="RSA";
            break;
        case SSL_aDSS:
            au="DSS";
            break;
        case SSL_aDH:
            au="DH";
            break;
        case SSL_aKRB5:
            au="KRB5";
            break;
        case SSL_aECDH:
            au = "ECDH";
            break;
        case SSL_aNULL:
            au="None";
            break;
        case SSL_aECDSA:
            au="ECDSA";
            break;
        case SSL_aPSK:
            au="PSK";
            break;
        default:
            au="unknown";
            break;
    }
    return au;
}

/**
 * Verify the X509 certificate via SSL_CTX_set_cert_verify_callback
 */
static int cert_verify_callback(X509_STORE_CTX* x509_store_ctx, void* arg __attribute__ ((unused)))
{
    /* Get the correct index to the SSLobject stored into X509_STORE_CTX. */
    SSL* ssl = (SSL*)X509_STORE_CTX_get_ex_data(x509_store_ctx, SSL_get_ex_data_X509_STORE_CTX_idx());
    JNI_TRACE("ssl=%p cert_verify_callback x509_store_ctx=%p arg=%p", ssl, x509_store_ctx, arg);

    AppData* appData = (AppData*) SSL_get_app_data(ssl);
    JNIEnv* env = appData->env;
    if (env == NULL) {
        LOGE("AppData->env missing in cert_verify_callback");
        JNI_TRACE("ssl=%p cert_verify_callback => 0", ssl);
        return 0;
    }
    jobject certificateChainVerifier = appData->certificateChainVerifier;

    jclass cls = env->GetObjectClass(certificateChainVerifier);
    jmethodID methodID = env->GetMethodID(cls, "verifyCertificateChain", "([[BLjava/lang/String;)V");

    jobjectArray objectArray = getCertificateBytes(env, x509_store_ctx->untrusted);

    const char* authMethod;
    switch (ssl->version) {
        case SSL2_VERSION:
            authMethod = "RSA";
            break;
        case SSL3_VERSION:
        case TLS1_VERSION:
        case DTLS1_VERSION:
            authMethod = SSL_CIPHER_authentication_method(ssl->s3->tmp.new_cipher);
            break;
        default:
            authMethod = "unknown";
          break;
    }
    jstring authMethodString = env->NewStringUTF(authMethod);

    env->CallVoidMethod(certificateChainVerifier, methodID, objectArray, authMethodString);

    int result = (env->ExceptionCheck()) ? 0 : 1;
    JNI_TRACE("ssl=%p cert_verify_callback => %d", ssl, result);
    return result;
}

/**
 * Call back to watch for handshake to be completed. This is necessary
 * for SSL_MODE_HANDSHAKE_CUTTHROUGH support, since SSL_do_handshake
 * returns before the handshake is completed in this case.
 */
static void info_callback(const SSL *ssl, int where, int ret __attribute__ ((unused))) {
    JNI_TRACE("ssl=%p info_callback where=0x%x ret=%d", ssl, where, ret);
#ifdef WITH_JNI_TRACE
    info_callback_LOG(ssl, where, ret);
#endif
    if (!(where & SSL_CB_HANDSHAKE_DONE)) {
      JNI_TRACE("ssl=%p info_callback ignored", ssl);
      return;
    }

    AppData* appData = (AppData*) SSL_get_app_data(ssl);
    JNIEnv* env = appData->env;
    if (env == NULL) {
        LOGE("AppData->env missing in info_callback");
        JNI_TRACE("ssl=%p info_callback env error", ssl);
        return;
    }
    jobject handshakeCompletedCallback = appData->handshakeCompletedCallback;

    jclass cls = env->GetObjectClass(handshakeCompletedCallback);
    jmethodID methodID = env->GetMethodID(cls, "handshakeCompleted", "()V");

    JNI_TRACE("ssl=%p info_callback calling handshakeCompleted", ssl);
    env->CallVoidMethod(handshakeCompletedCallback, methodID);

    if (env->ExceptionCheck()) {
      JNI_TRACE("ssl=%p info_callback exception", ssl);
    }

    appData->handshakeCompleted(env);
    JNI_TRACE("ssl=%p info_callback completed", ssl);
}

/*
 * public static native int SSL_CTX_new();
 */
static int NativeCrypto_SSL_CTX_new(JNIEnv* env, jclass) {
    SSL_CTX* sslCtx = SSL_CTX_new(SSLv23_method());
    if (sslCtx == NULL) {
        jniThrowRuntimeException(env, "SSL_CTX_new");
        return NULL;
    }
    // Note: We explicitly do not allow SSLv2 to be used.
    SSL_CTX_set_options(sslCtx, SSL_OP_ALL | SSL_OP_NO_SSLv2);

    int mode = SSL_CTX_get_mode(sslCtx);
    /*
     * Turn on "partial write" mode. This means that SSL_write() will
     * behave like Posix write() and possibly return after only
     * writing a partial buffer. Note: The alternative, perhaps
     * surprisingly, is not that SSL_write() always does full writes
     * but that it will force you to retry write calls having
     * preserved the full state of the original call. (This is icky
     * and undesirable.)
     */
    mode |= SSL_MODE_ENABLE_PARTIAL_WRITE;
#if defined(SSL_MODE_SMALL_BUFFERS) /* not all SSL versions have this */
    mode |= SSL_MODE_SMALL_BUFFERS;  /* lazily allocate record buffers; usually saves
                                      * 44k over the default */
#endif
#if defined(SSL_MODE_HANDSHAKE_CUTTHROUGH) /* not all SSL versions have this */
    mode |= SSL_MODE_HANDSHAKE_CUTTHROUGH;  /* enable sending of client data as soon as
                                             * ClientCCS and ClientFinished are sent */
#endif
    SSL_CTX_set_mode(sslCtx, mode);

    SSL_CTX_set_cert_verify_callback(sslCtx, cert_verify_callback, NULL);
    SSL_CTX_set_info_callback(sslCtx, info_callback);

#ifdef WITH_JNI_TRACE
    SSL_CTX_set_msg_callback(sslCtx, ssl_msg_callback_LOG); /* enable for message debug */
#endif
    JNI_TRACE("NativeCrypto_SSL_CTX_new => %p", sslCtx);
    return (jint) sslCtx;
}

/**
 * public static native void SSL_CTX_free(int ssl_ctx)
 */
static void NativeCrypto_SSL_CTX_free(JNIEnv* env,
        jclass, jint ssl_ctx_address)
{
    SSL_CTX* ssl_ctx = to_SSL_CTX(ssl_ctx_address);
    JNI_TRACE("ssl_ctx=%p NativeCrypto_SSL_CTX_free", ssl_ctx);
    if (ssl_ctx == NULL) {
        jniThrowNullPointerException(env, "SSL_CTX is null");
        return;
    }
    env->DeleteGlobalRef((jobject) ssl_ctx->app_verify_arg);
    SSL_CTX_free(ssl_ctx);
}

/**
 * Gets the chars of a String object as a '\0'-terminated UTF-8 string,
 * stored in a freshly-allocated BIO memory buffer.
 */
static BIO* stringToMemBuf(JNIEnv* env, jstring string) {
    jsize byteCount = env->GetStringUTFLength(string);
    LocalArray<1024> buf(byteCount + 1);
    env->GetStringUTFRegion(string, 0, env->GetStringLength(string), &buf[0]);

    BIO* bio = BIO_new(BIO_s_mem());
    if (bio == NULL) {
        jniThrowRuntimeException(env, "BIO_new failed");
        return NULL;
    }
    BIO_puts(bio, &buf[0]);
    return bio;
}

/**
 * public static native int SSL_new(int ssl_ctx, String privatekey, String certificate, byte[] seed,
 *                                  CertificateChainVerifier ccv) throws SSLException;
 */
static jint NativeCrypto_SSL_new(JNIEnv* env, jclass,
                                 jint ssl_ctx_address, jstring privatekey, jstring certificates, jbyteArray seed)
{
    SSL_CTX* ssl_ctx = to_SSL_CTX(ssl_ctx_address);
    JNI_TRACE("ssl_ctx=%p NativeCrypto_SSL_new privatekey=%p certificates=%p seed=%p",
             ssl_ctx, privatekey, certificates, seed);
    if (ssl_ctx == NULL) {
        jniThrowNullPointerException(env, "SSL_CTX is null");
        JNI_TRACE("ssl_ctx=%p NativeCrypto_SSL_new => NULL", ssl_ctx);
        return NULL;
    }

    // 'seed == null' when no SecureRandom Object is set
    // in the SSLContext.
    if (seed != NULL) {
        jbyte* randseed = env->GetByteArrayElements(seed, NULL);
        RAND_seed((unsigned char*) randseed, 1024);
        env->ReleaseByteArrayElements(seed, randseed, 0);
    } else {
        RAND_load_file("/dev/urandom", 1024);
    }

    SSL* ssl = SSL_new(ssl_ctx);
    if (ssl == NULL) {
        throwSSLExceptionWithSslErrors(env, ssl, 0,
                "Unable to create SSL structure");
        JNI_TRACE("ssl_ctx=%p NativeCrypto_SSL_new => NULL", ssl_ctx);
        return NULL;
    }

    /* Java code in class OpenSSLSocketImpl does the verification. Meaning of
     * SSL_VERIFY_NONE flag in client mode: if not using an anonymous cipher
     * (by default disabled), the server will send a certificate which will
     * be checked. The result of the certificate verification process can be
     * checked after the TLS/SSL handshake using the SSL_get_verify_result(3)
     * function. The handshake will be continued regardless of the
     * verification result.
     */
    SSL_set_verify(ssl, SSL_VERIFY_NONE, NULL);

    if (privatekey != NULL) {
        BIO* privatekeybio = stringToMemBuf(env, (jstring) privatekey);
        EVP_PKEY* privatekeyevp =
          PEM_read_bio_PrivateKey(privatekeybio, NULL, 0, NULL);
        BIO_free(privatekeybio);

        if (privatekeyevp == NULL) {
            LOGE(ERR_error_string(ERR_get_error(), NULL));
            throwSSLExceptionWithSslErrors(env, ssl, 0,
                    "Error parsing the private key");
            SSL_free(ssl);
            JNI_TRACE("ssl_ctx=%p NativeCrypto_SSL_new => NULL", ssl_ctx);
            return NULL;
        }

        BIO* certificatesbio = stringToMemBuf(env, (jstring) certificates);
        X509* certificatesx509 =
          PEM_read_bio_X509(certificatesbio, NULL, 0, NULL);
        BIO_free(certificatesbio);

        if (certificatesx509 == NULL) {
            LOGE(ERR_error_string(ERR_get_error(), NULL));
            throwSSLExceptionWithSslErrors(env, ssl, 0,
                    "Error parsing the certificates");
            EVP_PKEY_free(privatekeyevp);
            SSL_free(ssl);
            JNI_TRACE("ssl_ctx=%p NativeCrypto_SSL_new => NULL", ssl_ctx);
            return NULL;
        }

        int ret = SSL_use_certificate(ssl, certificatesx509);
        if (ret != 1) {
            LOGE(ERR_error_string(ERR_get_error(), NULL));
            throwSSLExceptionWithSslErrors(env, ssl, 0,
                    "Error setting the certificates");
            X509_free(certificatesx509);
            EVP_PKEY_free(privatekeyevp);
            SSL_free(ssl);
            JNI_TRACE("ssl_ctx=%p NativeCrypto_SSL_new => NULL", ssl_ctx);
            return NULL;
        }

        ret = SSL_use_PrivateKey(ssl, privatekeyevp);
        if (ret != 1) {
            LOGE(ERR_error_string(ERR_get_error(), NULL));
            throwSSLExceptionWithSslErrors(env, ssl, 0,
                    "Error setting the private key");
            X509_free(certificatesx509);
            EVP_PKEY_free(privatekeyevp);
            SSL_free(ssl);
            JNI_TRACE("ssl_ctx=%p NativeCrypto_SSL_new => NULL", ssl_ctx);
            return NULL;
        }

        ret = SSL_check_private_key(ssl);
        if (ret != 1) {
            throwSSLExceptionWithSslErrors(env, ssl, 0,
                    "Error checking the private key");
            X509_free(certificatesx509);
            EVP_PKEY_free(privatekeyevp);
            SSL_free(ssl);
            JNI_TRACE("ssl_ctx=%p NativeCrypto_SSL_new => NULL", ssl_ctx);
            return NULL;
        }
    }
    JNI_TRACE("ssl_ctx=%p NativeCrypto_SSL_new => ssl=%p", ssl_ctx, ssl);
    return (jint)ssl;
}

/**
 * public static native long SSL_get_mode(int ssl);
 */
static jlong NativeCrypto_SSL_get_mode(JNIEnv* env, jclass,
        jint ssl_address) {
    SSL* ssl = to_SSL(env, ssl_address, true);
    JNI_TRACE("ssl=%p NativeCrypto_SSL_get_mode", ssl);
    if (ssl == NULL) {
      JNI_TRACE("ssl=%p NativeCrypto_SSL_get_mode => 0", ssl);
      return 0;
    }
    long mode = SSL_get_mode(ssl);
    JNI_TRACE("ssl=%p NativeCrypto_SSL_get_mode => 0x%lx", ssl, mode);
    return mode;
}

/**
 * public static native long SSL_set_mode(int ssl, long mode);
 */
static jlong NativeCrypto_SSL_set_mode(JNIEnv* env, jclass,
        jint ssl_address, jlong mode) {
    SSL* ssl = to_SSL(env, ssl_address, true);
    JNI_TRACE("ssl=%p NativeCrypto_SSL_set_mode mode=0x%llx", ssl, mode);
    if (ssl == NULL) {
      return 0;
    }
    long result = SSL_set_mode(ssl, mode);
    JNI_TRACE("ssl=%p NativeCrypto_SSL_set_mode => 0x%lx", ssl, result);
    return result;
}

/**
 * public static native long SSL_clear_mode(int ssl, long mode);
 */
static jlong NativeCrypto_SSL_clear_mode(JNIEnv* env, jclass,
        jint ssl_address, jlong mode) {
    SSL* ssl = to_SSL(env, ssl_address, true);
    JNI_TRACE("ssl=%p NativeCrypto_SSL_clear_mode mode=0x%llx", ssl, mode);
    if (ssl == NULL) {
      return 0;
    }
    long result = SSL_clear_mode(ssl, mode);
    JNI_TRACE("ssl=%p NativeCrypto_SSL_clear_mode => 0x%lx", ssl, result);
    return result;
}

/**
 * public static native long SSL_get_options(int ssl);
 */
static jlong NativeCrypto_SSL_get_options(JNIEnv* env, jclass,
        jint ssl_address) {
    SSL* ssl = to_SSL(env, ssl_address, true);
    JNI_TRACE("ssl=%p NativeCrypto_SSL_get_options", ssl);
    if (ssl == NULL) {
      JNI_TRACE("ssl=%p NativeCrypto_SSL_get_options => 0", ssl);
      return 0;
    }
    long options = SSL_get_options(ssl);
    JNI_TRACE("ssl=%p NativeCrypto_SSL_get_options => 0x%lx", ssl, options);
    return options;
}

/**
 * public static native long SSL_set_options(int ssl, long options);
 */
static jlong NativeCrypto_SSL_set_options(JNIEnv* env, jclass,
        jint ssl_address, jlong options) {
    SSL* ssl = to_SSL(env, ssl_address, true);
    JNI_TRACE("ssl=%p NativeCrypto_SSL_set_options options=0x%llx", ssl, options);
    if (ssl == NULL) {
      return 0;
    }
    long result = SSL_set_options(ssl, options);
    JNI_TRACE("ssl=%p NativeCrypto_SSL_set_options => 0x%lx", ssl, result);
    return result;
}

/**
 * public static native long SSL_clear_options(int ssl, long options);
 */
static jlong NativeCrypto_SSL_clear_options(JNIEnv* env, jclass,
        jint ssl_address, jlong options) {
    SSL* ssl = to_SSL(env, ssl_address, true);
    JNI_TRACE("ssl=%p NativeCrypto_SSL_clear_options options=0x%llx", ssl, options);
    if (ssl == NULL) {
      return 0;
    }
    long result = SSL_clear_options(ssl, options);
    JNI_TRACE("ssl=%p NativeCrypto_SSL_clear_options => 0x%lx", ssl, result);
    return result;
}

/**
 * Sets the ciphers suites that are enabled in the SSL
 */
static void NativeCrypto_SSL_set_cipher_lists(JNIEnv* env, jclass,
        jint ssl_address, jobjectArray cipherSuites)
{
    SSL* ssl = to_SSL(env, ssl_address, true);
    JNI_TRACE("ssl=%p NativeCrypto_SSL_set_cipher_lists cipherSuites=%p", ssl, cipherSuites);
    if (ssl == NULL) {
        return;
    }

    STACK_OF(SSL_CIPHER)* cipherstack = sk_SSL_CIPHER_new_null();
    if (cipherstack == NULL) {
        jniThrowRuntimeException(env, "sk_SSL_CIPHER_new_null failed");
        return;
    }

    const SSL_METHOD* ssl_method = ssl->method;
    int num_ciphers = ssl_method->num_ciphers();

    int length = env->GetArrayLength(cipherSuites);
    JNI_TRACE("ssl=%p NativeCrypto_SSL_set_cipher_lists length=%d", ssl, length);
    for (int i = 0; i < length; i++) {
        ScopedLocalRef<jstring> cipherSuite(env,
                reinterpret_cast<jstring>(env->GetObjectArrayElement(cipherSuites, i)));
        ScopedUtfChars c(env, cipherSuite.get());
        JNI_TRACE("ssl=%p NativeCrypto_SSL_set_cipher_lists cipherSuite=%s", ssl, c.c_str());
        bool found = false;
        for (int j = 0; j < num_ciphers; j++) {
            const SSL_CIPHER* cipher = ssl_method->get_cipher(j);
            if ((strcmp(c.c_str(), cipher->name) == 0)
                    && (strcmp(SSL_CIPHER_get_version(cipher), "SSLv2"))) {
                sk_SSL_CIPHER_push(cipherstack, cipher);
                found = true;
            }
        }
        if (!found) {
            sk_SSL_CIPHER_free(cipherstack);
            jniThrowException(env, "java/lang/IllegalArgumentException",
                              "Could not find cipher suite.");
            return;
        }
    }

    int rc = SSL_set_cipher_lists(ssl, cipherstack);
    if (rc == 0) {
        freeSslErrorState();
        jniThrowException(env, "java/lang/IllegalArgumentException",
                          "Illegal cipher suite strings.");
    }
}

/**
 * Sets certificate expectations, especially for server to request client auth
 */
static void NativeCrypto_SSL_set_verify(JNIEnv* env,
        jclass, jint ssl_address, jint mode)
{
    SSL* ssl = to_SSL(env, ssl_address, true);
    JNI_TRACE("ssl=%p NativeCrypto_SSL_set_verify", ssl);
    if (ssl == NULL) {
      return;
    }
    SSL_set_verify(ssl, (int)mode, NULL);
}

/**
 * Sets the ciphers suites that are enabled in the SSL
 */
static void NativeCrypto_SSL_set_session(JNIEnv* env, jclass,
        jint ssl_address, jint ssl_session_address)
{
    SSL* ssl = to_SSL(env, ssl_address, true);
    SSL_SESSION* ssl_session = to_SSL_SESSION(ssl_session_address);
    JNI_TRACE("ssl=%p NativeCrypto_SSL_set_session ssl_session=%p", ssl, ssl_session);
    if (ssl == NULL) {
        return;
    }

    int ret = SSL_set_session(ssl, ssl_session);
    if (ret != 1) {
        /*
         * Translate the error, and throw if it turns out to be a real
         * problem.
         */
        int sslErrorCode = SSL_get_error(ssl, ret);
        if (sslErrorCode != SSL_ERROR_ZERO_RETURN) {
            throwSSLExceptionWithSslErrors(env, ssl, sslErrorCode,
                                          "SSL session set");
            SSL_clear(ssl);
        }
    }
}

/**
 * Sets the ciphers suites that are enabled in the SSL
 */
static void NativeCrypto_SSL_set_session_creation_enabled(JNIEnv* env, jclass,
        jint ssl_address, jboolean creation_enabled)
{
    SSL* ssl = to_SSL(env, ssl_address, true);
    JNI_TRACE("ssl=%p NativeCrypto_SSL_set_session_creation_enabled creation_enabled=%d", ssl, creation_enabled);
    if (ssl == NULL) {
        return;
    }
    SSL_set_session_creation_enabled(ssl, creation_enabled);
}

/**
 * Module scope variables initialized during JNI registration.
 */
static jfieldID field_Socket_mImpl;
static jfieldID field_Socket_mFD;

/**
 * Perform SSL handshake
 */
static jint NativeCrypto_SSL_do_handshake(JNIEnv* env, jclass,
    jint ssl_address, jobject socketObject, jobject ccv, jobject hcc, jint timeout, jboolean client_mode)
{
    SSL* ssl = to_SSL(env, ssl_address, true);
    JNI_TRACE("ssl=%p NativeCrypto_SSL_do_handshake socketObject=%p ccv=%p timeout=%d client_mode=%d",
             ssl, socketObject, ccv, timeout, client_mode);
    if (ssl == NULL) {
      JNI_TRACE("ssl=%p NativeCrypto_SSL_do_handshake => 0", ssl);
      return 0;
    }

    if (socketObject == NULL) {
        jniThrowNullPointerException(env, "Socket is null");
        JNI_TRACE("ssl=%p NativeCrypto_SSL_do_handshake => 0", ssl);
        return 0;
    }
    if (ccv == NULL) {
        jniThrowNullPointerException(env, "CertificateChainVerifier is null");
        JNI_TRACE("ssl=%p NativeCrypto_SSL_do_handshake => 0", ssl);
        return 0;
    }

    jobject socketImplObject = env->GetObjectField(socketObject, field_Socket_mImpl);
    if (socketImplObject == NULL) {
        throwSSLExceptionStr(env,
            "couldn't get the socket impl from the socket");
        JNI_TRACE("ssl=%p NativeCrypto_SSL_do_handshake => 0", ssl);
        return 0;
    }

    jobject fdObject = env->GetObjectField(socketImplObject, field_Socket_mFD);
    if (fdObject == NULL) {
        throwSSLExceptionStr(env,
            "couldn't get the file descriptor from the socket impl");
        JNI_TRACE("ssl=%p NativeCrypto_SSL_do_handshake => 0", ssl);
        return 0;
    }

    int fd = jniGetFDFromFileDescriptor(env, fdObject);
    if (fd == -1) {
        throwSSLExceptionStr(env, "Invalid file descriptor");
        SSL_clear(ssl);
        JNI_TRACE("ssl=%p NativeCrypto_SSL_do_handshake => 0", ssl);
        return 0;
    }

    int ret = SSL_set_fd(ssl, fd);
    JNI_TRACE("ssl=%p NativeCrypto_SSL_do_handshake s=%d", ssl, fd);

    if (ret != 1) {
        throwSSLExceptionWithSslErrors(env, ssl, 0,
                "Error setting the file descriptor");
        SSL_clear(ssl);
        JNI_TRACE("ssl=%p NativeCrypto_SSL_do_handshake => 0", ssl);
        return 0;
    }

    /*
     * Make socket non-blocking, so SSL_connect SSL_read() and SSL_write() don't hang
     * forever and we can use select() to find out if the socket is ready.
     */
    int mode = fcntl(fd, F_GETFL);
    if (mode == -1 || fcntl(fd, F_SETFL, mode | O_NONBLOCK) == -1) {
        throwSSLExceptionStr(env, "Unable to make socket non blocking");
        SSL_clear(ssl);
        JNI_TRACE("ssl=%p NativeCrypto_SSL_do_handshake => 0", ssl);
        return 0;
    }

    /*
     * Create our special application data.
     */
    AppData* appData = AppData::create(env, ccv, hcc);
    if (appData == NULL) {
        throwSSLExceptionStr(env, "Unable to create application data");
        SSL_clear(ssl);
        JNI_TRACE("ssl=%p NativeCrypto_SSL_do_handshake => 0", ssl);
        return 0;
    }
    SSL_set_app_data(ssl, (char*) appData);
    JNI_TRACE("ssl=%p AppData::create => %p", ssl, appData);

    if (client_mode) {
        SSL_set_connect_state(ssl);
    } else {
        SSL_set_accept_state(ssl);
    }

    while (appData->aliveAndKicking) {
        errno = 0;
        appData->setEnv(env);
        ret = SSL_do_handshake(ssl);
        appData->clearEnv();
        // cert_verify_callback threw exception
        if (env->ExceptionCheck()) {
          SSL_clear(ssl);
          JNI_TRACE("ssl=%p NativeCrypto_SSL_do_handshake => 0", ssl);
          return 0;
        }
        if (ret == 1) {
            break;
        } else if (errno == EINTR) {
            continue;
        } else {
            // LOGD("SSL_connect: result %d, errno %d, timeout %d", ret, errno, timeout);
            int error = SSL_get_error(ssl, ret);

            /*
             * If SSL_connect doesn't succeed due to the socket being
             * either unreadable or unwritable, we use sslSelect to
             * wait for it to become ready. If that doesn't happen
             * before the specified timeout or an error occurs, we
             * cancel the handshake. Otherwise we try the SSL_connect
             * again.
             */
            if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
                appData->waitingThreads++;
                int selectResult = sslSelect(error, fd, appData, timeout);

                if (selectResult == -1) {
                    throwSSLExceptionWithSslErrors(env, ssl, error, "handshake error");
                    SSL_clear(ssl);
                    JNI_TRACE("ssl=%p NativeCrypto_SSL_do_handshake => 0", ssl);
                    return 0;
                } else if (selectResult == 0) {
                    throwSocketTimeoutException(env, "SSL handshake timed out");
                    SSL_clear(ssl);
                    freeSslErrorState();
                    JNI_TRACE("ssl=%p NativeCrypto_SSL_do_handshake => 0", ssl);
                    return 0;
                }
            } else {
                // LOGE("Unknown error %d during handshake", error);
                break;
            }
        }
    }

    if (ret == 0) {
        /*
         * The other side closed the socket before the handshake could be
         * completed, but everything is within the bounds of the TLS protocol.
         * We still might want to find out the real reason of the failure.
         */
        int sslErrorCode = SSL_get_error(ssl, ret);
        if (sslErrorCode == SSL_ERROR_NONE ||
            (sslErrorCode == SSL_ERROR_SYSCALL && errno == 0)) {
          throwSSLExceptionStr(env, "Connection closed by peer");
        } else {
          throwSSLExceptionWithSslErrors(env, ssl, sslErrorCode,
              "Trouble with SSL handshake");
        }
        SSL_clear(ssl);
        JNI_TRACE("ssl=%p NativeCrypto_SSL_do_handshake => 0", ssl);
        return 0;
    }
    if (ret < 0) {
        /*
         * Translate the error and throw exception. We are sure it is an error
         * at this point.
         */
        int sslErrorCode = SSL_get_error(ssl, ret);
        throwSSLExceptionWithSslErrors(env, ssl, sslErrorCode,
                "Trouble with SSL handshake");
        SSL_clear(ssl);
        JNI_TRACE("ssl=%p NativeCrypto_SSL_do_handshake => 0", ssl);
        return 0;
    }
    SSL_SESSION* ssl_session = SSL_get1_session(ssl);
    JNI_TRACE("ssl=%p NativeCrypto_SSL_do_handshake => ssl_session=%p", ssl, ssl_session);
    return (jint) ssl_session;
}

/**
 * public static native byte[][] SSL_get_certificate(int ssl);
 */
static jobjectArray NativeCrypto_SSL_get_certificate(JNIEnv* env, jclass, jint ssl_address)
{
    SSL* ssl = to_SSL(env, ssl_address, true);
    JNI_TRACE("ssl=%p NativeCrypto_SSL_get_certificate", ssl);
    if (ssl == NULL) {
      JNI_TRACE("ssl=%p NativeCrypto_SSL_get_certificate => NULL", ssl);
      return NULL;
    }
    X509* certificate = SSL_get_certificate(ssl);
    if (certificate == NULL) {
      JNI_TRACE("ssl=%p NativeCrypto_SSL_get_certificate => NULL", ssl);
      return NULL;
    }
    // TODO convert from single certificate to chain properly.  One
    // option would be to have the chain remembered where
    // SSL_use_certificate is used. Another would be to save the
    // intermediate CAs with SSL_CTX SSL_CTX_add_extra_chain_cert.
    STACK_OF(X509)* chain = sk_X509_new_null();
    if (chain == NULL) {
      jniThrowRuntimeException(env, "Unable to allocate local certificate chain");
      JNI_TRACE("ssl=%p NativeCrypto_SSL_get_certificate => NULL", ssl);
      return NULL;
    }
    sk_X509_push(chain, certificate);
    jobjectArray objectArray = getCertificateBytes(env, chain);
    sk_X509_free(chain);
    JNI_TRACE("ssl=%p NativeCrypto_SSL_get_certificate => %p", ssl, objectArray);
    return objectArray;
}


/**
 * Helper function which does the actual reading. The Java layer guarantees that
 * at most one thread will enter this function at any given time.
 *
 * @param ssl non-null; the SSL context
 * @param buf non-null; buffer to read into
 * @param len length of the buffer, in bytes
 * @param sslReturnCode original SSL return code
 * @param sslErrorCode filled in with the SSL error code in case of error
 * @return number of bytes read on success, -1 if the connection was
 * cleanly shut down, or THROW_EXCEPTION if an exception should be thrown.
 */
static int sslRead(JNIEnv* env, SSL* ssl, char* buf, jint len, int* sslReturnCode,
        int* sslErrorCode, int timeout) {

    // LOGD("Entering sslRead, caller requests to read %d bytes...", len);

    if (len == 0) {
        // Don't bother doing anything in this case.
        return 0;
    }

    int fd = SSL_get_fd(ssl);
    BIO* bio = SSL_get_rbio(ssl);

    AppData* appData = (AppData*) SSL_get_app_data(ssl);

    while (appData->aliveAndKicking) {
        errno = 0;

        // Lock
        if (MUTEX_LOCK(appData->mutex) == -1) {
            return -1;
        }

        unsigned int bytesMoved = BIO_number_read(bio) + BIO_number_written(bio);

        // LOGD("Doing SSL_Read()");
        AppData* appData = (AppData*) SSL_get_app_data(ssl);
        appData->setEnv(env);
        int result = SSL_read(ssl, buf, len);
        appData->clearEnv();
        int error = SSL_ERROR_NONE;
        if (result <= 0) {
            error = SSL_get_error(ssl, result);
            freeSslErrorState();
        }
        // LOGD("Returned from SSL_Read() with result %d, error code %d", result, error);

        // If we have been successful in moving data around, check whether it
        // might make sense to wake up other blocked threads, so they can give
        // it a try, too.
        if (BIO_number_read(bio) + BIO_number_written(bio) != bytesMoved && appData->waitingThreads > 0) {
            sslNotify(appData);
        }

        // If we are blocked by the underlying socket, tell the world that
        // there will be one more waiting thread now.
        if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
            appData->waitingThreads++;
        }

        // Unlock
        MUTEX_UNLOCK(appData->mutex);

        switch (error) {
             // Sucessfully read at least one byte.
            case SSL_ERROR_NONE: {
                return result;
            }

            // Read zero bytes. End of stream reached.
            case SSL_ERROR_ZERO_RETURN: {
                return -1;
            }

            // Need to wait for availability of underlying layer, then retry.
            case SSL_ERROR_WANT_READ:
            case SSL_ERROR_WANT_WRITE: {
                int selectResult = sslSelect(error, fd, appData, timeout);
                if (selectResult == -1) {
                    *sslReturnCode = -1;
                    *sslErrorCode = error;
                    return THROW_EXCEPTION;
                } else if (selectResult == 0) {
                    return THROW_SOCKETTIMEOUTEXCEPTION;
                }

                break;
            }

            // A problem occured during a system call, but this is not
            // necessarily an error.
            case SSL_ERROR_SYSCALL: {
                // Connection closed without proper shutdown. Tell caller we
                // have reached end-of-stream.
                if (result == 0) {
                    return -1;
                }

                // System call has been interrupted. Simply retry.
                if (errno == EINTR) {
                    break;
                }

                // Note that for all other system call errors we fall through
                // to the default case, which results in an Exception.
            }

            // Everything else is basically an error.
            default: {
                *sslReturnCode = result;
                *sslErrorCode = error;
                return THROW_EXCEPTION;
            }
        }
    }

    return -1;
}

/**
 * OpenSSL read function (1): only one chunk is read (returned as jint).
 */
static jint NativeCrypto_SSL_read_byte(JNIEnv* env, jclass, jint ssl_address, jint timeout)
{
    SSL* ssl = to_SSL(env, ssl_address, true);
    JNI_TRACE("ssl=%p NativeCrypto_SSL_read_byte timeout=%d", ssl, timeout);
    if (ssl == NULL) {
        return 0;
    }

    unsigned char byteRead;
    int returnCode = 0;
    int errorCode = 0;

    int ret = sslRead(env, ssl, (char *) &byteRead, 1, &returnCode, &errorCode, timeout);

    int result;
    switch (ret) {
        case THROW_EXCEPTION:
            // See sslRead() regarding improper failure to handle normal cases.
            throwSSLExceptionWithSslErrors(env, ssl, errorCode, "Read error");
            result = -1;
            break;
        case THROW_SOCKETTIMEOUTEXCEPTION:
            throwSocketTimeoutException(env, "Read timed out");
            result = -1;
            break;
        case -1:
            // Propagate EOF upwards.
            result = -1;
            break;
        default:
            // Return the actual char read, make sure it stays 8 bits wide.
            result = ((jint) byteRead) & 0xFF;
            break;
    }
    JNI_TRACE("ssl=%p NativeCrypto_SSL_read_byte => %d", ssl, result);
    return result;
}

/**
 * OpenSSL read function (2): read into buffer at offset n chunks.
 * Returns 1 (success) or value <= 0 (failure).
 */
static jint NativeCrypto_SSL_read(JNIEnv* env, jclass, jint ssl_address, jbyteArray dest, jint offset, jint len, jint timeout)
{
    SSL* ssl = to_SSL(env, ssl_address, true);
    JNI_TRACE("ssl=%p NativeCrypto_SSL_read dest=%p offset=%d len=%d timeout=%d", ssl, dest, offset, len, timeout);
    if (ssl == NULL) {
        return 0;
    }

    jbyte* bytes = env->GetByteArrayElements(dest, NULL);
    int returnCode = 0;
    int errorCode = 0;

    int ret = sslRead(env, ssl, (char*) (bytes + offset), len, &returnCode, &errorCode, timeout);

    env->ReleaseByteArrayElements(dest, bytes, 0);

    int result;
    if (ret == THROW_EXCEPTION) {
        // See sslRead() regarding improper failure to handle normal cases.
        throwSSLExceptionWithSslErrors(env, ssl, errorCode,
                "Read error");
        result = -1;
    } else if(ret == THROW_SOCKETTIMEOUTEXCEPTION) {
        throwSocketTimeoutException(env, "Read timed out");
        result = -1;
    } else {
        result = ret;
    }

    JNI_TRACE("ssl=%p NativeCrypto_SSL_read => %d", ssl, result);
    return result;
}

/**
 * Helper function which does the actual writing. The Java layer guarantees that
 * at most one thread will enter this function at any given time.
 *
 * @param ssl non-null; the SSL context
 * @param buf non-null; buffer to write
 * @param len length of the buffer, in bytes
 * @param sslReturnCode original SSL return code
 * @param sslErrorCode filled in with the SSL error code in case of error
 * @return number of bytes read on success, -1 if the connection was
 * cleanly shut down, or THROW_EXCEPTION if an exception should be thrown.
 */
static int sslWrite(JNIEnv* env, SSL* ssl, const char* buf, jint len, int* sslReturnCode,
        int* sslErrorCode) {

    // LOGD("Entering sslWrite(), caller requests to write %d bytes...", len);

    if (len == 0) {
        // Don't bother doing anything in this case.
        return 0;
    }

    int fd = SSL_get_fd(ssl);
    BIO* bio = SSL_get_wbio(ssl);

    AppData* appData = (AppData*) SSL_get_app_data(ssl);

    int count = len;

    while (appData->aliveAndKicking && len > 0) {
        errno = 0;
        if (MUTEX_LOCK(appData->mutex) == -1) {
            return -1;
        }

        unsigned int bytesMoved = BIO_number_read(bio) + BIO_number_written(bio);

        // LOGD("Doing SSL_write() with %d bytes to go", len);
        appData->setEnv(env);
        int result = SSL_write(ssl, buf, len);
        appData->clearEnv();
        int error = SSL_ERROR_NONE;
        if (result <= 0) {
            error = SSL_get_error(ssl, result);
            freeSslErrorState();
        }
        // LOGD("Returned from SSL_write() with result %d, error code %d", result, error);

        // If we have been successful in moving data around, check whether it
        // might make sense to wake up other blocked threads, so they can give
        // it a try, too.
        if (BIO_number_read(bio) + BIO_number_written(bio) != bytesMoved && appData->waitingThreads > 0) {
            sslNotify(appData);
        }

        // If we are blocked by the underlying socket, tell the world that
        // there will be one more waiting thread now.
        if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
            appData->waitingThreads++;
        }

        MUTEX_UNLOCK(appData->mutex);

        switch (error) {
             // Sucessfully write at least one byte.
            case SSL_ERROR_NONE: {
                buf += result;
                len -= result;
                break;
            }

            // Wrote zero bytes. End of stream reached.
            case SSL_ERROR_ZERO_RETURN: {
                return -1;
            }

            // Need to wait for availability of underlying layer, then retry.
            // The concept of a write timeout doesn't really make sense, and
            // it's also not standard Java behavior, so we wait forever here.
            case SSL_ERROR_WANT_READ:
            case SSL_ERROR_WANT_WRITE: {
                int selectResult = sslSelect(error, fd, appData, 0);
                if (selectResult == -1) {
                    *sslReturnCode = -1;
                    *sslErrorCode = error;
                    return THROW_EXCEPTION;
                } else if (selectResult == 0) {
                    return THROW_SOCKETTIMEOUTEXCEPTION;
                }

                break;
            }

            // An problem occured during a system call, but this is not
            // necessarily an error.
            case SSL_ERROR_SYSCALL: {
                // Connection closed without proper shutdown. Tell caller we
                // have reached end-of-stream.
                if (result == 0) {
                    return -1;
                }

                // System call has been interrupted. Simply retry.
                if (errno == EINTR) {
                    break;
                }

                // Note that for all other system call errors we fall through
                // to the default case, which results in an Exception.
            }

            // Everything else is basically an error.
            default: {
                *sslReturnCode = result;
                *sslErrorCode = error;
                return THROW_EXCEPTION;
            }
        }
    }
    // LOGD("Successfully wrote %d bytes", count);

    return count;
}

/**
 * OpenSSL write function (1): only one chunk is written.
 */
static void NativeCrypto_SSL_write_byte(JNIEnv* env, jclass, jint ssl_address, jint b)
{
    SSL* ssl = to_SSL(env, ssl_address, true);
    JNI_TRACE("ssl=%p NativeCrypto_SSL_write_byte b=%d", ssl, b);
    if (ssl == NULL) {
        return;
    }

    int returnCode = 0;
    int errorCode = 0;
    char buf[1] = { (char) b };
    int ret = sslWrite(env, ssl, buf, 1, &returnCode, &errorCode);

    if (ret == THROW_EXCEPTION) {
        // See sslWrite() regarding improper failure to handle normal cases.
        throwSSLExceptionWithSslErrors(env, ssl, errorCode,
                "Write error");
    } else if(ret == THROW_SOCKETTIMEOUTEXCEPTION) {
        throwSocketTimeoutException(env, "Write timed out");
    }
}

/**
 * OpenSSL write function (2): write into buffer at offset n chunks.
 */
static void NativeCrypto_SSL_write(JNIEnv* env, jclass,
        jint ssl_address, jbyteArray dest, jint offset, jint len)
{
    SSL* ssl = to_SSL(env, ssl_address, true);
    JNI_TRACE("ssl=%p NativeCrypto_SSL_write dest=%p offset=%d len=%d", ssl, dest, offset, len);
    if (ssl == NULL) {
        return;
    }

    ScopedByteArray bytes(env, dest);
    int returnCode = 0;
    int errorCode = 0;
    int ret = sslWrite(env, ssl, (const char *) (bytes.get() + offset), len, &returnCode, &errorCode);

    if (ret == THROW_EXCEPTION) {
        // See sslWrite() regarding improper failure to handle normal cases.
        throwSSLExceptionWithSslErrors(env, ssl, errorCode, "Write error");
    } else if(ret == THROW_SOCKETTIMEOUTEXCEPTION) {
        throwSocketTimeoutException(env, "Write timed out");
    }
}

/**
 * Interrupt any pending IO before closing the socket.
 */
static void NativeCrypto_SSL_interrupt(
        JNIEnv* env, jclass, jint ssl_address) {
    SSL* ssl = to_SSL(env, ssl_address, false);
    JNI_TRACE("ssl=%p NativeCrypto_SSL_interrupt", ssl);
    if (ssl == NULL) {
        return;
    }

    /*
     * Mark the connection as quasi-dead, then send something to the emergency
     * file descriptor, so any blocking select() calls are woken up.
     */
    AppData* appData = (AppData*) SSL_get_app_data(ssl);
    if (appData != NULL) {
        appData->aliveAndKicking = 0;

        // At most two threads can be waiting.
        sslNotify(appData);
        sslNotify(appData);
    }
}

/**
 * OpenSSL close SSL socket function.
 */
static void NativeCrypto_SSL_shutdown(
        JNIEnv* env, jclass, jint ssl_address) {
    SSL* ssl = to_SSL(env, ssl_address, false);
    JNI_TRACE("ssl=%p NativeCrypto_SSL_shutdown", ssl);
    if (ssl == NULL) {
        return;
    }
    /*
     * Try to make socket blocking again. OpenSSL literature recommends this.
     */
    int fd = SSL_get_fd(ssl);
    JNI_TRACE("ssl=%p NativeCrypto_SSL_shutdown s=%d", ssl, fd);
    if (fd != -1) {
        int mode = fcntl(fd, F_GETFL);
        if (mode == -1 || fcntl(fd, F_SETFL, mode & ~O_NONBLOCK) == -1) {
//            throwSSLExceptionStr(env, "Unable to make socket blocking again");
//            LOGW("Unable to make socket blocking again");
        }
    }

    AppData* appData = (AppData*) SSL_get_app_data(ssl);
    appData->setEnv(env);
    int ret = SSL_shutdown(ssl);
    appData->clearEnv();
    switch (ret) {
        case 0:
            /*
             * Shutdown was not successful (yet), but there also
             * is no error. Since we can't know whether the remote
             * server is actually still there, and we don't want to
             * get stuck forever in a second SSL_shutdown() call, we
             * simply return. This is not security a problem as long
             * as we close the underlying socket, which we actually
             * do, because that's where we are just coming from.
             */
            break;
        case 1:
            /*
             * Shutdown was sucessful. We can safely return. Hooray!
             */
            break;
        default:
            /*
             * Everything else is a real error condition. We should
             * let the Java layer know about this by throwing an
             * exception.
             */
            int sslErrorCode = SSL_get_error(ssl, ret);
            throwSSLExceptionWithSslErrors(env, ssl, sslErrorCode, "SSL shutdown failed");
            break;
    }

    SSL_clear(ssl);
    freeSslErrorState();
}

/**
 * public static native void SSL_free(int ssl);
 */
static void NativeCrypto_SSL_free(JNIEnv* env, jclass, jint ssl_address)
{
    SSL* ssl = to_SSL(env, ssl_address, true);
    JNI_TRACE("ssl=%p NativeCrypto_SSL_free", ssl);
    if (ssl == NULL) {
        return;
    }
    AppData* appData = (AppData*) SSL_get_app_data(ssl);
    SSL_set_app_data(ssl, NULL);
    JNI_TRACE("ssl=%p AppData::destroy(%p)", ssl, appData);
    AppData::destroy(env, appData);
    SSL_free(ssl);
}

/**
 * Gets and returns in a byte array the ID of the actual SSL session.
 */
static jbyteArray NativeCrypto_SSL_SESSION_session_id(JNIEnv* env, jclass, jint ssl_session_address) {
    SSL_SESSION* ssl_session = to_SSL_SESSION(ssl_session_address);
    JNI_TRACE("ssl_session=%p NativeCrypto_SSL_SESSION_session_id", ssl_session);
    jbyteArray result = env->NewByteArray(ssl_session->session_id_length);
    if (result != NULL) {
        jbyte* src = reinterpret_cast<jbyte*>(ssl_session->session_id);
        env->SetByteArrayRegion(result, 0, ssl_session->session_id_length, src);
    }
    JNI_TRACE("ssl_session=%p NativeCrypto_SSL_SESSION_session_id => %p session_id_length=%d",
             ssl_session, result, ssl_session->session_id_length);
    return result;
}

/**
 * Our implementation of what might be considered
 * SSL_SESSION_get_peer_cert_chain
 *
 */
// TODO move to jsse.patch
static STACK_OF(X509)* SSL_SESSION_get_peer_cert_chain(SSL_CTX* ssl_ctx, SSL_SESSION* ssl_session) {
    SSL* ssl = SSL_new(ssl_ctx);
    if (ssl == NULL) {
        return NULL;
    }
    SSL_set_session(ssl, ssl_session);
    STACK_OF(X509)* chain = SSL_get_peer_cert_chain(ssl);
    SSL_free(ssl);
    return chain;
}

// Fills a byte[][] with the peer certificates in the chain.
static jobjectArray NativeCrypto_SSL_SESSION_get_peer_cert_chain(JNIEnv* env,
        jclass, jint ssl_ctx_address, jint ssl_session_address)
{
    SSL_CTX* ssl_ctx = to_SSL_CTX(ssl_ctx_address);
    SSL_SESSION* ssl_session = to_SSL_SESSION(ssl_session_address);
    JNI_TRACE("ssl_session=%p NativeCrypto_SSL_SESSION_get_peer_cert_chain ssl_ctx=%p", ssl_session, ssl_ctx);
    if (ssl_ctx == NULL) {
        jniThrowNullPointerException(env, "SSL_CTX is null");
        JNI_TRACE("ssl_session=%p NativeCrypto_SSL_SESSION_get_peer_cert_chain => NULL", ssl_session);
        return NULL;
    }
    STACK_OF(X509)* chain = SSL_SESSION_get_peer_cert_chain(ssl_ctx, ssl_session);
    jobjectArray objectArray = getCertificateBytes(env, chain);
    JNI_TRACE("ssl_session=%p NativeCrypto_SSL_SESSION_get_peer_cert_chain => %p", ssl_session, objectArray);
    return objectArray;
}

/**
 * Gets and returns in a long integer the creation's time of the
 * actual SSL session.
 */
static jlong NativeCrypto_SSL_SESSION_get_time(JNIEnv*, jclass, jint ssl_session_address) {
    SSL_SESSION* ssl_session = to_SSL_SESSION(ssl_session_address);
    JNI_TRACE("ssl_session=%p NativeCrypto_SSL_SESSION_get_time", ssl_session);
    jlong result = SSL_SESSION_get_time(ssl_session); // must be jlong, not long or *1000 will overflow
    result *= 1000; // OpenSSL uses seconds, Java uses milliseconds.
    JNI_TRACE("ssl_session=%p NativeCrypto_SSL_SESSION_get_time => %lld", ssl_session, result);
    return result;
}

/**
 * Our implementation of what might be considered
 * SSL_SESSION_get_version, based on SSL_get_version.
 * See get_ssl_version above.
 */
// TODO move to jsse.patch
static const char* SSL_SESSION_get_version(SSL_SESSION* ssl_session) {
  return get_ssl_version(ssl_session->ssl_version);
}

/**
 * Gets and returns in a string the version of the SSL protocol. If it
 * returns the string "unknown" it means that no connection is established.
 */
static jstring NativeCrypto_SSL_SESSION_get_version(JNIEnv* env, jclass, jint ssl_session_address) {
    SSL_SESSION* ssl_session = to_SSL_SESSION(ssl_session_address);
    JNI_TRACE("ssl_session=%p NativeCrypto_SSL_SESSION_get_version", ssl_session);
    const char* protocol = SSL_SESSION_get_version(ssl_session);
    JNI_TRACE("ssl_session=%p NativeCrypto_SSL_SESSION_get_version => %s", ssl_session, protocol);
    return env->NewStringUTF(protocol);
}

/**
 * Gets and returns in a string the set of ciphers the actual SSL session uses.
 */
static jstring NativeCrypto_SSL_SESSION_cipher(JNIEnv* env, jclass, jint ssl_session_address) {
    SSL_SESSION* ssl_session = to_SSL_SESSION(ssl_session_address);
    JNI_TRACE("ssl_session=%p NativeCrypto_SSL_SESSION_cipher", ssl_session);
    const SSL_CIPHER* cipher = ssl_session->cipher;
    const char* name = SSL_CIPHER_get_name(cipher);
    JNI_TRACE("ssl_session=%p NativeCrypto_SSL_SESSION_cipher => %s", ssl_session, name);
    return env->NewStringUTF(name);
}

/**
 * Frees the SSL session.
 */
static void NativeCrypto_SSL_SESSION_free(JNIEnv*, jclass, jint ssl_session_address) {
    SSL_SESSION* ssl_session = to_SSL_SESSION(ssl_session_address);
    JNI_TRACE("ssl_session=%p NativeCrypto_SSL_SESSION_free", ssl_session);
    SSL_SESSION_free(ssl_session);
}


/**
 * Serializes the native state of the session (ID, cipher, and keys but
 * not certificates). Returns a byte[] containing the DER-encoded state.
 * See apache mod_ssl.
 */
static jbyteArray NativeCrypto_i2d_SSL_SESSION(JNIEnv* env, jclass, jint ssl_session_address) {
    SSL_SESSION* ssl_session = to_SSL_SESSION(ssl_session_address);
    JNI_TRACE("ssl_session=%p NativeCrypto_i2d_SSL_SESSION", ssl_session);
    if (ssl_session == NULL) {
        JNI_TRACE("ssl_session=%p NativeCrypto_i2d_SSL_SESSION => NULL", ssl_session);
        return NULL;
    }

    // Compute the size of the DER data
    int size = i2d_SSL_SESSION(ssl_session, NULL);
    if (size == 0) {
        JNI_TRACE("ssl_session=%p NativeCrypto_i2d_SSL_SESSION => NULL", ssl_session);
        return NULL;
    }

    jbyteArray bytes = env->NewByteArray(size);
    if (bytes != NULL) {
        jbyte* tmp = env->GetByteArrayElements(bytes, NULL);
        unsigned char* ucp = reinterpret_cast<unsigned char*>(tmp);
        i2d_SSL_SESSION(ssl_session, &ucp);
        env->ReleaseByteArrayElements(bytes, tmp, 0);
    }

    JNI_TRACE("ssl_session=%p NativeCrypto_i2d_SSL_SESSION => size=%d", ssl_session, size);
    return bytes;
}

/**
 * Deserialize the session.
 */
static jint NativeCrypto_d2i_SSL_SESSION(JNIEnv* env, jclass, jbyteArray bytes, jint size) {
    JNI_TRACE("NativeCrypto_d2i_SSL_SESSION bytes=%p size=%d", bytes, size);
    if (bytes == NULL) {
        JNI_TRACE("NativeCrypto_d2i_SSL_SESSION => 0");
        return 0;
    }

    ScopedByteArray tmp(env, bytes);
    const unsigned char* ucp = reinterpret_cast<const unsigned char*>(tmp.get());
    SSL_SESSION* ssl_session = d2i_SSL_SESSION(NULL, &ucp, size);

    JNI_TRACE("NativeCrypto_d2i_SSL_SESSION => %p", ssl_session);
    return static_cast<jint>(reinterpret_cast<uintptr_t>(ssl_session));
}

/*
 * Defines the mapping from Java methods and their signatures
 * to native functions. Order is (1) Java name, (2) signature,
 * (3) pointer to C function.
 */
static JNINativeMethod sNativeCryptoMethods[] = {
    { "clinit",               "()V",           (void*)NativeCrypto_clinit},
    { "EVP_PKEY_new_DSA",     "([B[B[B[B[B)I", (void*)NativeCrypto_EVP_PKEY_new_DSA },
    { "EVP_PKEY_new_RSA",     "([B[B[B[B[B)I", (void*)NativeCrypto_EVP_PKEY_new_RSA },
    { "EVP_PKEY_free",        "(I)V",          (void*)NativeCrypto_EVP_PKEY_free },
    { "EVP_new",              "()I",           (void*)NativeCrypto_EVP_new },
    { "EVP_free",             "(I)V",          (void*)NativeCrypto_EVP_free },
    { "EVP_DigestFinal",      "(I[BI)I",       (void*)NativeCrypto_EVP_DigestFinal },
    { "EVP_DigestInit",       "(ILjava/lang/String;)V", (void*)NativeCrypto_EVP_DigestInit },
    { "EVP_DigestBlockSize",  "(I)I",          (void*)NativeCrypto_EVP_DigestBlockSize },
    { "EVP_DigestSize",       "(I)I",          (void*)NativeCrypto_EVP_DigestSize },
    { "EVP_DigestUpdate",     "(I[BII)V",      (void*)NativeCrypto_EVP_DigestUpdate },
    { "EVP_VerifyInit",       "(ILjava/lang/String;)V", (void*)NativeCrypto_EVP_VerifyInit },
    { "EVP_VerifyUpdate",     "(I[BII)V",      (void*)NativeCrypto_EVP_VerifyUpdate },
    { "EVP_VerifyFinal",      "(I[BIII)I",     (void*)NativeCrypto_EVP_VerifyFinal },
    { "verifySignature",      "([B[BLjava/lang/String;[B[B)I", (void*)NativeCrypto_verifysignature},
    { "SSL_CTX_new",          "()I",           (void*)NativeCrypto_SSL_CTX_new },
    { "SSL_CTX_free",         "(I)V",          (void*)NativeCrypto_SSL_CTX_free },
    { "SSL_new",              "(ILjava/lang/String;Ljava/lang/String;[B)I", (void*)NativeCrypto_SSL_new},
    { "SSL_get_mode",         "(I)J",          (void*)NativeCrypto_SSL_get_mode },
    { "SSL_set_mode",         "(IJ)J",         (void*)NativeCrypto_SSL_set_mode },
    { "SSL_clear_mode",       "(IJ)J",         (void*)NativeCrypto_SSL_clear_mode },
    { "SSL_get_options",      "(I)J",          (void*)NativeCrypto_SSL_get_options },
    { "SSL_set_options",      "(IJ)J",         (void*)NativeCrypto_SSL_set_options },
    { "SSL_clear_options",    "(IJ)J",         (void*)NativeCrypto_SSL_clear_options },
    { "SSL_set_cipher_lists", "(I[Ljava/lang/String;)V", (void*)NativeCrypto_SSL_set_cipher_lists },
    { "SSL_set_verify",       "(II)V",         (void*)NativeCrypto_SSL_set_verify},
    { "SSL_set_session",      "(II)V",         (void*)NativeCrypto_SSL_set_session },
    { "SSL_set_session_creation_enabled", "(IZ)V", (void*)NativeCrypto_SSL_set_session_creation_enabled },
    { "SSL_do_handshake",     "(ILjava/net/Socket;Lorg/apache/harmony/xnet/provider/jsse/NativeCrypto$CertificateChainVerifier;Lorg/apache/harmony/xnet/provider/jsse/NativeCrypto$HandshakeCompletedCallback;IZ)I",(void*)NativeCrypto_SSL_do_handshake},
    { "SSL_get_certificate",  "(I)[[B",        (void*)NativeCrypto_SSL_get_certificate},
    { "SSL_read_byte",        "(II)I",         (void*)NativeCrypto_SSL_read_byte},
    { "SSL_read",             "(I[BIII)I",     (void*)NativeCrypto_SSL_read},
    { "SSL_write_byte",       "(II)V",         (void*)NativeCrypto_SSL_write_byte},
    { "SSL_write",            "(I[BII)V",      (void*)NativeCrypto_SSL_write},
    { "SSL_interrupt",        "(I)V",          (void*)NativeCrypto_SSL_interrupt},
    { "SSL_shutdown",         "(I)V",          (void*)NativeCrypto_SSL_shutdown},
    { "SSL_free",             "(I)V",          (void*)NativeCrypto_SSL_free},
    { "SSL_SESSION_session_id", "(I)[B",       (void*)NativeCrypto_SSL_SESSION_session_id },
    { "SSL_SESSION_get_peer_cert_chain", "(II)[[B", (void*)NativeCrypto_SSL_SESSION_get_peer_cert_chain },
    { "SSL_SESSION_get_time", "(I)J",          (void*)NativeCrypto_SSL_SESSION_get_time },
    { "SSL_SESSION_get_version", "(I)Ljava/lang/String;", (void*)NativeCrypto_SSL_SESSION_get_version },
    { "SSL_SESSION_cipher",   "(I)Ljava/lang/String;", (void*)NativeCrypto_SSL_SESSION_cipher },
    { "SSL_SESSION_free",     "(I)V",          (void*)NativeCrypto_SSL_SESSION_free },
    { "i2d_SSL_SESSION",      "(I)[B",         (void*)NativeCrypto_i2d_SSL_SESSION },
    { "d2i_SSL_SESSION",      "([BI)I",        (void*)NativeCrypto_d2i_SSL_SESSION },
};

int register_org_apache_harmony_xnet_provider_jsse_NativeCrypto(JNIEnv* env) {
    JNI_TRACE("register_org_apache_harmony_xnet_provider_jsse_NativeCrypto");
    // Register org.apache.harmony.xnet.provider.jsse.NativeCrypto methods
    int result = jniRegisterNativeMethods(env,
                                          "org/apache/harmony/xnet/provider/jsse/NativeCrypto",
                                          sNativeCryptoMethods,
                                          NELEM(sNativeCryptoMethods));
    if (result == -1) {
        return -1;
    }

    // java.net.Socket
    jclass socket = env->FindClass("java/net/Socket");
    if (socket == NULL) {
        LOGE("Can't find class java.net.Socket");
        return -1;
    }
    field_Socket_mImpl = env->GetFieldID(socket, "impl", "Ljava/net/SocketImpl;");
    if (field_Socket_mImpl == NULL) {
        LOGE("Can't find field impl in class java.net.Socket");
        return -1;
    }

    // java.net.SocketImpl
    jclass socketImplClass = env->FindClass("java/net/SocketImpl");
    if (socketImplClass == NULL) {
        LOGE("Can't find class java.net.SocketImpl");
        return -1;
    }
    field_Socket_mFD = env->GetFieldID(socketImplClass, "fd", "Ljava/io/FileDescriptor;");
    if (field_Socket_mFD == NULL) {
        LOGE("Can't find field fd in java.net.SocketImpl");
        return -1;
    }

    return 0;
}
