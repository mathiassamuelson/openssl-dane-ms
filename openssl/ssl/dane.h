#include <openssl/ssl.h>
#include <openssl/bio.h>



int dane_verify(SSL *con, char *s_host, short s_port);
int dane_verify_cb(int ok, X509_STORE_CTX *store);
