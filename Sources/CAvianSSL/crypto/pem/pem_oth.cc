/*
 * Copyright 1995-2016 The OpenSSL Project Authors. All Rights Reserved.
 *
 * Licensed under the OpenSSL license (the "License").  You may not use
 * this file except in compliance with the License.  You can obtain a copy
 * in the file LICENSE in the source distribution or at
 * https://www.openssl.org/source/license.html
 */

#include <CAvianSSL_pem.h>

#include <stdio.h>

#include <CAvianSSL_err.h>
#include <CAvianSSL_evp.h>
#include <CAvianSSL_mem.h>
#include <CAvianSSL_obj.h>
#include <CAvianSSL_rand.h>
#include <CAvianSSL_x509.h>

// Handle 'other' PEMs: not private keys

void *PEM_ASN1_read_bio(d2i_of_void *d2i, const char *name, BIO *bp, void **x,
                        pem_password_cb *cb, void *u) {
  const unsigned char *p = NULL;
  unsigned char *data = NULL;
  long len;
  char *ret = NULL;

  if (!PEM_bytes_read_bio(&data, &len, NULL, name, bp, cb, u)) {
    return NULL;
  }
  p = data;
  ret = reinterpret_cast<char *>(d2i(x, &p, len));
  if (ret == NULL) {
    OPENSSL_PUT_ERROR(PEM, ERR_R_ASN1_LIB);
  }
  OPENSSL_free(data);
  return ret;
}
