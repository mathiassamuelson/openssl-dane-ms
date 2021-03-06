#include <stdio.h>
#include <openssl/dane.h>
#include <openssl/bio.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <errno.h>
#include <arpa/inet.h>
#include <unbound.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>

BIO *b_err;
int get_tlsa(struct ub_result *result, char *s_host, short s_port);
int ca_constraint(const SSL *con, const X509 *tlsa_cert, int usage);
int service_cert_constraint(const X509 *con_cert, const X509 *tlsa_cert);

int dane_verify_cb(int ok, X509_STORE_CTX *store) {
	struct ub_result *dns_result;
	struct ub_ctx* ctx;
	char dns_name[256];
	X509 *cert;
	SSL *con;
	typedef struct {
		int verbose_mode;
		int verify_depth;
		int always_continue;
	} mydata_t;
	int mydata_index;
	mydata_t *mydata;
	int retval, err, depth;
	
	if (b_err == NULL)
		b_err=BIO_new_fp(stderr,BIO_NOCLOSE);
	
	cert = X509_STORE_CTX_get_current_cert(store);
	err = X509_STORE_CTX_get_error(store);
	depth = X509_STORE_CTX_get_error_depth(ctx);
	
	int ssl_idx = SSL_get_ex_data_X509_STORE_CTX_idx();
	if (ssl_idx < 0) {
		BIO_printf(b_err, "DANE failed to find SSL index: %d\n", ssl_idx);
		return -1;
	}
	con = X509_STORE_CTX_get_ex_data(store, SSL_get_ex_data_X509_STORE_CTX_idx());
	mydata = SSL_get_ex_data(con, mydata_index);
	
	int peerfd;
	peerfd = SSL_get_fd(con);
	socklen_t len;
	struct sockaddr_storage addr;
	char ipstr[INET6_ADDRSTRLEN];
	char node[NI_MAXHOST];
	int port;

	len = sizeof addr;
	getpeername(peerfd, (struct sockaddr*)&addr, &len);

	// deal with both IPv4 and IPv6:
	if (addr.ss_family == AF_INET) {
	    struct sockaddr_in *s = (struct sockaddr_in *)&addr;
	    port = ntohs(s->sin_port);
	    inet_ntop(AF_INET, &s->sin_addr, ipstr, sizeof ipstr);
		struct sockaddr_in sa;
		sa.sin_family = AF_INET;
		inet_pton(AF_INET, ipstr, &sa.sin_addr);
		int res = getnameinfo((struct sockaddr*)&sa, sizeof(sa), node, sizeof(node), NULL, 0, 0);
	} else { // AF_INET6
		struct sockaddr_in6 *s = (struct sockaddr_in6 *)&addr;
	    port = ntohs(s->sin6_port);
	    inet_ntop(AF_INET6, &s->sin6_addr, ipstr, sizeof ipstr);
	}

	BIO_printf(b_err, "Peer IP address: %s\n", ipstr);
	BIO_printf(b_err, "Peer port      : %d\n", port);
	BIO_printf(b_err, "Peer hostname  : %s\n", node);
		
	ctx = ub_ctx_create();
	if(!ctx) {
		printf("error: could not create unbound context\n");
		return -1;
	}
	if( (retval=ub_ctx_resolvconf(ctx, "/etc/resolv.conf")) != 0) {
		printf("error reading resolv.conf: %s. errno says: %s\n", 
			ub_strerror(retval), strerror(errno));
		return -1;
	}
	if( (retval=ub_ctx_hosts(ctx, "/etc/hosts")) != 0) {
		printf("error reading hosts: %s. errno says: %s\n", 
			ub_strerror(retval), strerror(errno));
		return -1;
	}
	
	retval = sprintf(dns_name, "_%d._tcp.%s", port, node);
	if(retval < 0) {
		printf("failure to create dns name\n");
		return -1;
	}
	BIO_printf(b_err,"DANE dane_verify_cb() dns name: %s\n", dns_name);
	retval = ub_resolve(ctx, dns_name, 65534, 1, &dns_result);
	if(retval != 0) {
		BIO_printf(b_err, "resolve error: %s\n", ub_strerror(retval));
		return -1;
	}
	
	if(dns_result->havedata) {
		int i;
		for (i = 0; dns_result->data[i] != NULL; i++) {
			unsigned char usage, selector, matching_type;
			unsigned char *tlsa_bytes;
			
			if (dns_result->len[i] < 35) {
				// must have at least 1+1+1+32 bytes for the SHA-256 case
				BIO_printf(b_err, "DANE: Not enough data: %d available\n",
					dns_result->len[i]);
				return -1;
			}
			unsigned char *rdata = (unsigned char *)dns_result->data[i];
			usage = (char) *rdata++;
			selector = (char) *rdata++;
			matching_type = (char) *rdata++;
			tlsa_bytes = (unsigned char *) rdata;
			X509 *tlsa_cert;
			tlsa_cert = d2i_X509(NULL, &tlsa_bytes, dns_result->len[i]-3);
			
			BIO_printf(b_err, "DANE: Usage %d Selector %d Matching Type %d\n",
				usage, selector, matching_type);
			
			if (selector != 0)
				continue;
			if (matching_type != 0)
				continue;
			
			if (usage == 0 || usage == 2) {
				int retval;
				retval = ca_constraint(con, tlsa_cert, usage);
				if (retval == 0)
					BIO_printf(b_err, "DANE dane_verify_cb() Passed validation for usage %d\n", usage);
				else
					BIO_printf(b_err, "DANE dane_verify_cb() Failed validation for usage %d\n", usage);
				return retval;
			}
			if (usage == 1) {
				int retval;
				retval = service_cert_constraint(cert, tlsa_cert);
				if (retval == 0)
					BIO_printf(b_err, "DANE dane_verify_cb() Passed validation for usage %d\n", usage);
				else
					BIO_printf(b_err, "DANE dane_verify_cb() Failed validation for usage %d\n", usage);
				return retval;
			}
		}
	}
	return ok;
}

int dane_verify(SSL *con, char *s_host, short s_port) {
	struct ub_result *dns_result;
	struct ub_ctx* ctx;
	char dns_name[256];
	int retval;
	
	if (b_err == NULL)
		b_err=BIO_new_fp(stderr,BIO_NOCLOSE);
	BIO_printf(b_err, "DANE:%s:%d\n", s_host, s_port);
	
	int peerfd;
	peerfd = SSL_get_fd(con);
	socklen_t len;
	struct sockaddr_storage addr;
	char ipstr[INET6_ADDRSTRLEN];
	char node[NI_MAXHOST];
	int port;

	len = sizeof addr;
	getpeername(peerfd, (struct sockaddr*)&addr, &len);

	// deal with both IPv4 and IPv6:
	if (addr.ss_family == AF_INET) {
	    struct sockaddr_in *s = (struct sockaddr_in *)&addr;
	    port = ntohs(s->sin_port);
	    inet_ntop(AF_INET, &s->sin_addr, ipstr, sizeof ipstr);
		struct sockaddr_in sa;
		sa.sin_family = AF_INET;
		inet_pton(AF_INET, ipstr, &sa.sin_addr);
		int res = getnameinfo((struct sockaddr*)&sa, sizeof(sa), node, sizeof(node), NULL, 0, 0);
	} else { // AF_INET6
		struct sockaddr_in6 *s = (struct sockaddr_in6 *)&addr;
	    port = ntohs(s->sin6_port);
	    inet_ntop(AF_INET6, &s->sin6_addr, ipstr, sizeof ipstr);
	}

	BIO_printf(b_err, "Peer IP address: %s\n", ipstr);
	BIO_printf(b_err, "Peer port      : %d\n", port);
	BIO_printf(b_err, "Peer hostname  : %s\n", node);
	
	ctx = ub_ctx_create();
	if(!ctx) {
		printf("error: could not create unbound context\n");
		return -1;
	}
	if( (retval=ub_ctx_resolvconf(ctx, "/etc/resolv.conf")) != 0) {
		printf("error reading resolv.conf: %s. errno says: %s\n", 
			ub_strerror(retval), strerror(errno));
		return -1;
	}
	if( (retval=ub_ctx_hosts(ctx, "/etc/hosts")) != 0) {
		printf("error reading hosts: %s. errno says: %s\n", 
			ub_strerror(retval), strerror(errno));
		return -1;
	}
	
	retval = sprintf(dns_name, "_%d._tcp.%s", s_port, s_host );
	if(retval < 0) {
		printf("failure to create dns name\n");
		return -1;
	}
	BIO_printf(b_err,"DANE:dns name: %s\n", dns_name);
	retval = ub_resolve(ctx, dns_name, 65534, 1, &dns_result);
	if(retval != 0) {
		printf("resolve error: %s\n", ub_strerror(retval));
		return -1;
	}
	
	if(dns_result->havedata) {
		int i;
		for (i = 0; dns_result->data[i] != NULL; i++) {
			unsigned char usage, selector, matching_type;
			unsigned char *tlsa_bytes;
			
			if (dns_result->len[i] < 35) {
				// must have at least 1+1+1+32 bytes for the SHA-256 case
				BIO_printf(b_err, "DANE: Not enough data: %d available\n",
					dns_result->len[i]);
				return -1;
			}
			unsigned char *rdata = (unsigned char *)dns_result->data[i];
			usage = (char) *rdata++;
			selector = (char) *rdata++;
			matching_type = (char) *rdata++;
			tlsa_bytes = (unsigned char *)rdata;
			X509 *tlsa_cert;
			tlsa_cert = d2i_X509(NULL, &tlsa_bytes, dns_result->len[i]-3);
			
			BIO_printf(b_err, "DANE: Usage %d Selector %d Matching Type %d\n",
				usage, selector, matching_type);
			
			if (selector != 0)
				continue;
			if (matching_type != 0)
				continue;
			
			if (usage == 0 || usage == 2) {
				int retval;
				retval = ca_constraint(con, tlsa_cert, usage);
				return retval;
			}
			if (usage == 1) {
				X509 *cert = NULL;
				cert = SSL_get_peer_certificate(con);
				int retval;
				retval = service_cert_constraint(cert, tlsa_cert);
				if (retval == 0)
					BIO_printf(b_err, "DANE: Passed validation for usage 1\n");
				else
					BIO_printf(b_err, "DANE: Failed validation for usage 1\n");
				return retval;
			}
		}
	} else
		return 0;

	(void)BIO_flush(b_err);
	return 0;
}

/*
 *	Returns: 	0 if successfully matching certificate to TLSA record bytes
 *				-1 if there was no match
 */
int ca_constraint(const SSL *con, const X509 *tlsa_cert, int usage) {
	STACK_OF(X509) *cert_chain = NULL;
	cert_chain = SSL_get_peer_cert_chain(con);
	BIO_printf(b_err, "DANE ca_constraint() chain of %d length\n", 
		sk_X509_num(cert_chain));
	int ret_val;
	ret_val = 0;
	
	if (cert_chain != NULL) {
		int i;
		for (i = 0; i < sk_X509_num(cert_chain); i++) {			
			BIO_printf(b_err, "DANE ca_constraint() cert %d of %d.\n",
				i, sk_X509_num(cert_chain));
			/*
			BIO_printf(b_err, "DANE CXN Certificate\n");
			PEM_write_bio_X509(b_err, sk_X509_value(cert_chain, i));
			BIO_printf(b_err, "DANE TLSA Certificate\n");
			PEM_write_bio_X509(b_err, tlsa_cert);
			*/
			if (X509_cmp(tlsa_cert, sk_X509_value(cert_chain, i)) < 0) {
				ret_val = -1;
				BIO_printf(b_err, "DANE ca_constraint() certificates didn't match\n");
			} else {
				BIO_printf(b_err, "DANE ca_constraint() certificates matches\n");
				if (usage == 0)
					return 0;
				
				/*	For this to be a trust anchor, the following characteristics applies:
				 * 	1. Issuer name is the same as Subject name
				 * 	2. Either or both
				 *	a) the Key Usage field is set to keyCertSign (KU_KEY_CERT_SIGN)
				 *	b) the basicConstraints field has the attribute cA set to True (EXFLAG_CA)
				 */
				X509_NAME *issuer_name, *subject_name;
				issuer_name = X509_get_issuer_name(tlsa_cert);
				subject_name = X509_get_subject_name(tlsa_cert);
				
				if (X509_name_cmp(issuer_name, subject_name) == 0) {
					BIO_printf(b_err, "DANE issuer == subject\n");
					
					if (tlsa_cert->ex_flags & EXFLAG_CA) {
						BIO_printf(b_err, "DANE ca_constraint() EXFLAG_CA set\n");
						return 0;
					}
/*	Left unimplemented since I don't have a CA certificate to work with.*/
					int ext_count, j;
					ext_count = X509_get_ext_count(tlsa_cert);
					BIO_printf(b_err, "DANE ca_constraint() %d certificate extensions\n");

				} else {
					return 0;
				}
			}
		}
	}
	return ret_val;
}

/*
 *	Returns: 	0 if successfully matching certificate to TLSA record bytes
 *				-1 if there was no match
 */
//int service_cert_constraint(const SSL *con, const unsigned char *tlsa_bytes, int tlsa_len) {
int service_cert_constraint(const X509 *con_cert, const X509 *tlsa_cert) {
	int ret_val;
	ret_val = 0;
	
	if (con_cert != NULL) {
		if (X509_cmp(tlsa_cert, con_cert) != 0) {
			ret_val = -1;
			BIO_printf(b_err, "DANE server_cert_constraint() certificates didn't match\n");
		} else {
			BIO_printf(b_err, "DANE server_cert_constraint() certificates matches\n");
			// Must return immediately in case there are non-matching certificates
			return 0;
		}
	} else
		BIO_printf(b_err,"DANE:no peer certificate available\n");
		
	return ret_val;
}
