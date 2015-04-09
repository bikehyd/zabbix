/*
** Zabbix
** Copyright (C) 2001-2015 Zabbix SIA
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
**/

#include "common.h"
#include "comms.h"
#include "log.h"
#include "tls.h"
#include "tls_tcp.h"
#include "tls_tcp_active.h"

#if defined(HAVE_POLARSSL)
#	include <polarssl/entropy.h>
#	include <polarssl/ctr_drbg.h>
#	include <polarssl/ssl.h>
#	include <polarssl/error.h>
#	include <polarssl/debug.h>
#elif defined(HAVE_GNUTLS)
#	include <gnutls/gnutls.h>
#	include <gnutls/x509.h>
#elif defined(HAVE_OPENSSL)
#	include <openssl/ssl.h>
#	include <openssl/err.h>
#	include <openssl/rand.h>
#endif

/* Currently use only TLS 1.2, which has number 3.3. In 2015 a new standard for TLS 1.3 is expected. */
/* Then we might need to support both TLS 1.2 and 1.3 to work with older Zabbix agents. */
#if defined(HAVE_POLARSSL)
#	define ZBX_TLS_MIN_MAJOR_VER	SSL_MAJOR_VERSION_3
#	define ZBX_TLS_MIN_MINOR_VER	SSL_MINOR_VERSION_3
#	define ZBX_TLS_MAX_MAJOR_VER	SSL_MAJOR_VERSION_3
#	define ZBX_TLS_MAX_MINOR_VER	SSL_MINOR_VERSION_3
#	define ZBX_TLS_CIPHERSUITE_CERT	0			/* select only certificate ciphersuites */
#	define ZBX_TLS_CIPHERSUITE_PSK	1			/* select only pre-shared key ciphersuites */
#	define ZBX_TLS_CIPHERSUITE_ALL	2			/* select ciphersuites with certificate and PSK */
#endif

extern unsigned int	configured_tls_connect_mode;
extern unsigned int	configured_tls_accept_modes;

#if defined(HAVE_POLARSSL) || defined(HAVE_GNUTLS) || defined(HAVE_OPENSSL)
extern ZBX_THREAD_LOCAL unsigned char	process_type;
extern unsigned char			program_type;
extern char				*CONFIG_TLS_CONNECT;
extern char				*CONFIG_TLS_ACCEPT;
extern char				*CONFIG_TLS_CA_FILE;
extern char				*CONFIG_TLS_CRL_FILE;
extern char				*CONFIG_TLS_CERT_FILE;
extern char				*CONFIG_TLS_KEY_FILE;
extern char				*CONFIG_TLS_PSK_FILE;
extern char				*CONFIG_TLS_PSK_IDENTITY;

ZBX_THREAD_LOCAL static char		*my_psk			= NULL;
ZBX_THREAD_LOCAL static size_t		my_psk_len		= 0;
ZBX_THREAD_LOCAL static char		*my_psk_identity	= NULL;
ZBX_THREAD_LOCAL static size_t		my_psk_identity_len	= 0;

/* Pointer to DCget_psk_by_identity() initialized at runtime. This is a workaround for linking. */
/* Server and proxy link with src/libs/zbxdbcache/dbconfig.o where DCget_psk_by_identity() resides */
/* but other components (e.g. agent) do not link dbconfig.o. */
size_t			(*find_psk_in_cache)(const unsigned char *, unsigned char *, size_t) = NULL;
#endif

#if defined(HAVE_POLARSSL)
ZBX_THREAD_LOCAL static x509_crt		*ca_cert		= NULL;
ZBX_THREAD_LOCAL static x509_crl		*crl			= NULL;
ZBX_THREAD_LOCAL static x509_crt		*my_cert		= NULL;
ZBX_THREAD_LOCAL static pk_context		*my_priv_key		= NULL;
ZBX_THREAD_LOCAL static entropy_context		*entropy		= NULL;
ZBX_THREAD_LOCAL static ctr_drbg_context	*ctr_drbg		= NULL;
ZBX_THREAD_LOCAL static char			*err_msg		= NULL;
ZBX_THREAD_LOCAL static int			*ciphersuites_cert	= NULL;
ZBX_THREAD_LOCAL static int			*ciphersuites_psk	= NULL;
ZBX_THREAD_LOCAL static int			*ciphersuites_all	= NULL;
ZBX_THREAD_LOCAL static char			work_buf[SSL_MAX_CONTENT_LEN + 1];
#elif defined(HAVE_GNUTLS)
ZBX_THREAD_LOCAL static gnutls_certificate_credentials_t	my_cert_creds		= NULL;
ZBX_THREAD_LOCAL static gnutls_psk_client_credentials_t		my_psk_client_creds	= NULL;
ZBX_THREAD_LOCAL static gnutls_psk_server_credentials_t		my_psk_server_creds	= NULL;
ZBX_THREAD_LOCAL static gnutls_priority_t			ciphersuites_cert	= NULL;
ZBX_THREAD_LOCAL static gnutls_priority_t			ciphersuites_psk	= NULL;
ZBX_THREAD_LOCAL static gnutls_priority_t			ciphersuites_all	= NULL;
#elif defined(HAVE_OPENSSL)
ZBX_THREAD_LOCAL static const SSL_METHOD	*method			= NULL;
ZBX_THREAD_LOCAL static SSL_CTX			*ctx_cert		= NULL;
ZBX_THREAD_LOCAL static SSL_CTX			*ctx_psk		= NULL;
ZBX_THREAD_LOCAL static SSL_CTX			*ctx_all		= NULL;
/* variables for passing required PSK-identity and PSK info to client callback function */
ZBX_THREAD_LOCAL static char			*psk_identity_for_cb	= NULL;
ZBX_THREAD_LOCAL static size_t			psk_identity_len_for_cb	= 0;
ZBX_THREAD_LOCAL static char			*psk_for_cb		= NULL;
ZBX_THREAD_LOCAL static size_t			psk_len_for_cb		= 0;
/* buffer for messages produced by zbx_openssl_info_callback() */
ZBX_THREAD_LOCAL char				info_buf[256];
#endif

#if defined(HAVE_POLARSSL)
/******************************************************************************
 *                                                                            *
 * Function: zbx_tls_make_personalization_string                              *
 *                                                                            *
 * Purpose: provide additional entropy for initialization of crypto library   *
 *                                                                            *
 * Comments:                                                                  *
 *     For more information about why and how to use personalization strings  *
 *     see                                                                    *
 *     https://polarssl.org/module-level-design-rng                           *
 *     http://csrc.nist.gov/publications/nistpubs/800-90A/SP800-90A.pdf       *
 *                                                                            *
 ******************************************************************************/
static void	zbx_tls_make_personalization_string(char **pers, size_t *len)
{
	/* TODO: follow recommendations in http://csrc.nist.gov/publications/nistpubs/800-90A/SP800-90A.pdf */
	/* and add more entropy into the personalization string (e.g. process PID, microseconds of current time etc.) */
	/* Pay attention to the personalization string length as described in SP800-90A.pdf */

	/* For demo purposes only. TODO: replace with production code. */
#define DEMO_PERS_STRING	"Zabbix TLSZabbix TLSZabbix TLSZabbix TLSZabbix TLS"
	*pers = zbx_strdup(*pers, DEMO_PERS_STRING);
	*len = strlen(DEMO_PERS_STRING);
}
#endif

#if defined(HAVE_POLARSSL)
/******************************************************************************
 *                                                                            *
 * Function: polarssl_debug                                                   *
 *                                                                            *
 * Purpose: write a PolarSSL debug message into Zabbix log                    *
 *                                                                            *
 * Comments:                                                                  *
 *     Parameter 'tls_ctx' is not used but cannot be removed because this is  *
 *     a callback function, its arguments are defined in PolarSSL.            *
 ******************************************************************************/
static void	polarssl_debug(void *tls_ctx, int level, const char *str)
{
	char	msg[1024];	/* Apparently 1024 bytes is the longest message which can come from PolarSSL 1.3.9 */

	/* remove '\n' from the end of debug message */
	zbx_strlcpy(msg, str, sizeof(msg));
	zbx_rtrim(msg, "\n");
	zabbix_log(LOG_LEVEL_DEBUG, "PolarSSL debug: level=%d \"%s\"", level, msg);
}
#elif defined(HAVE_GNUTLS)
/******************************************************************************
 *                                                                            *
 * Function: zbx_gnutls_debug                                                 *
 *                                                                            *
 * Purpose: write a GnuTLS debug message into Zabbix log                      *
 *                                                                            *
 * Comments:                                                                  *
 *     This is a callback function, its arguments are defined in GnuTLS.      *
 ******************************************************************************/
static void	zbx_gnutls_debug(int level, const char* str)
{
	char	msg[1024];

	/* remove '\n' from the end of debug message */
	zbx_strlcpy(msg, str, sizeof(msg));
	zbx_rtrim(msg, "\n");
	zabbix_log(LOG_LEVEL_DEBUG, "GnuTLS debug: level=%d \"%s\"", level, msg);
}

#if GNUTLS_VERSION_NUMBER >= 0x030000
/******************************************************************************
 *                                                                            *
 * Function: zbx_gnutls_audit                                                 *
 *                                                                            *
 * Purpose: write a GnuTLS audit message into Zabbix log                      *
 *                                                                            *
 * Comments:                                                                  *
 *     This is a callback function, its arguments are defined in GnuTLS.      *
 ******************************************************************************/
static void	zbx_gnutls_audit(gnutls_session_t session, const char* str)
{
	zabbix_log(LOG_LEVEL_WARNING, "GnuTLS audit: \"%s\"", str);
}
#endif
#endif	/* defined(HAVE_GNUTLS) */

#if defined(HAVE_OPENSSL)
/******************************************************************************
 *                                                                            *
 * Function: zbx_openssl_info_callback                                        *
 *                                                                            *
 * Purpose: get state, alert, error information on TLS connection             *
 *                                                                            *
 * Comments:                                                                  *
 *     This is a callback function, its arguments are defined in OpenSSL,     *
 ******************************************************************************/
static void	zbx_openssl_info_callback(const SSL *ssl, int where, int ret)
{
	const char	*direction, *rw, *handshake;

	if (0 != (where & SSL_ST_CONNECT))
		direction = " connect";
	else if (0 != (where & SSL_ST_ACCEPT))
		direction = " accept";
	else
		direction = "";

	if (0 != (where & SSL_CB_READ))
		rw = " read";
	else if (0 != (where & SSL_CB_WRITE))
		rw = " write";
	else
		rw = "";

	if (0 != (where & SSL_CB_EXIT))
		handshake = " handshake";
	else
		handshake = "";

	/* There was an idea of using SSL_CB_LOOP and SSL_state_string_long() to write state changes into Zabbix log */
	/* if logging level is LOG_LEVEL_TRACE. Unfortunately if OpenSSL for security is compiled without SSLv3 */
	/* (i.e. OPENSSL_NO_SSL3 is defined) then SSL_state_string_long() does not provide descriptions of many */
	/* states anymore. The idea was dropped but the code is here for debugging hard problems. */
#if 0
	if (0 != (where & SSL_CB_LOOP) && SUCCEED == zabbix_check_log_level(LOG_LEVEL_TRACE))
	{
		zabbix_log(LOG_LEVEL_TRACE, "OpenSSL debug: state=0x%x \"%s\"", (unsigned int)SSL_state(ssl),
				SSL_state_string_long(ssl));
	}
#endif
	if (0 != (where & SSL_CB_ALERT))	/* alert sent or received */
	{
		zbx_snprintf(info_buf, sizeof(info_buf), "TLS%s%s%s %s alert \"%s\"", handshake, direction, rw,
				SSL_alert_type_string_long(ret), SSL_alert_desc_string_long(ret));
	}
}
#endif

/******************************************************************************
 *                                                                            *
 * Function: zbx_tls_error_msg                                                *
 *                                                                            *
 * Purpose: compose a TLS error message                                       *
 *                                                                            *
 ******************************************************************************/
#if defined(HAVE_POLARSSL)
static void	zbx_tls_error_msg(int error_code, const char *msg, char **error)
{
	char	err[128];	/* 128 bytes are enough for PolarSSL error messages */

	polarssl_strerror(error_code, err, sizeof(err));
	*error = zbx_dsprintf(*error, "%s%s", msg, err);
}
#elif defined(HAVE_OPENSSL)
void	zbx_tls_error_msg(char **error, size_t *error_alloc, size_t *error_offset)
{
	unsigned long	error_code;
	const char	*file, *data;
	int		line, flags;
	char		err[1024];

	/* concatenate all error messages in the queue into one string */

	while (0 != (error_code = ERR_get_error_line_data(&file, &line, &data, &flags)))
	{
		ERR_error_string_n(error_code, err, sizeof(err));

		zbx_snprintf_alloc(error, error_alloc, error_offset, " file %s line %d: %s", file, line, err);

		if (NULL != data && 0 != (flags & ERR_TXT_STRING))
			zbx_snprintf_alloc(error, error_alloc, error_offset, ": %s", data);
	}
}
#endif

#if defined(HAVE_POLARSSL) || defined(HAVE_GNUTLS) || defined(HAVE_OPENSSL)
/******************************************************************************
 *                                                                            *
 * Function: zbx_parameter_not_empty                                          *
 *                                                                            *
 * Purpose: enforce if a configuration parameter is defined it must not be    *
 *          empty                                                             *
 *                                                                            *
 ******************************************************************************/
static void	zbx_parameter_not_empty(const char *param, const char *param_name)
{
	if (NULL != param)
	{
		while ('\0' != *param)
		{
			if (0 == isspace(*param++))
				return;
		}

		zabbix_log(LOG_LEVEL_CRIT, "configuration parameter \"%s\" is defined but empty", param_name);
		zbx_tls_free();
		exit(EXIT_FAILURE);
	}
}
#endif

#if defined(HAVE_POLARSSL) || defined(HAVE_GNUTLS) || defined(HAVE_OPENSSL)
/******************************************************************************
 *                                                                            *
 * Function: zbx_tls_validate_config                                          *
 *                                                                            *
 * Purpose: check for allowed combinations of TLS configuration parameters    *
 *                                                                            *
 * Comments:                                                                  *
 *     Valid combinations:                                                    *
 *         - either all 3 certificate parameters - CONFIG_TLS_CERT_FILE,      *
 *           CONFIG_TLS_KEY_FILE, CONFIG_TLS_CA_FILE  - are defined and not   *
 *           empty or none of them. Parameter CONFIG_TLS_CRL_FILE is optional *
 *           but may be defined only together with the 3 certificate          *
 *           parameters,                                                      *
 *         - either both PSK parameters - CONFIG_TLS_PSK_IDENTITY and         *
 *           CONFIG_TLS_PSK_FILE - are defined and not empty or none of them, *
 *           (if CONFIG_TLS_PSK_IDENTITY is defined it must be a valid UTF-8  *
 *           string),                                                         *
 *         - in agentd and active proxy the certificate and PSK parameters    *
 *           must match the value of CONFIG_TLS_CONNECT parameter,            *
 *         - in agentd, agent and passive proxy the certificate and PSK       *
 *           parameters must match the value of CONFIG_TLS_ACCEPT parameter.  *
 *                                                                            *
 ******************************************************************************/
static void	zbx_tls_validate_config(void)
{
	/* parse and validate 'TLSConnect' parameter used in zabbix_proxy.conf, zabbix_agentd.conf */
	/* and '--tls-connect' parameter used in zabbix_get and zabbix_sender */

	if (0 != (program_type & (ZBX_PROGRAM_TYPE_GET | ZBX_PROGRAM_TYPE_SENDER)))
		zbx_parameter_not_empty(CONFIG_TLS_CONNECT, "--tls-connect");
	else
		zbx_parameter_not_empty(CONFIG_TLS_CONNECT, "TLSConnect");

	if (NULL != CONFIG_TLS_CONNECT)
	{
		/* 'configured_tls_connect_mode' is shared between threads on MS Windows */

		if (0 == strcmp(CONFIG_TLS_CONNECT, ZBX_TCP_SEC_UNENCRYPTED_TXT))
			configured_tls_connect_mode = ZBX_TCP_SEC_UNENCRYPTED;
		else if (0 == strcmp(CONFIG_TLS_CONNECT, ZBX_TCP_SEC_TLS_CERT_TXT))
			configured_tls_connect_mode = ZBX_TCP_SEC_TLS_CERT;
		else if (0 == strcmp(CONFIG_TLS_CONNECT, ZBX_TCP_SEC_TLS_PSK_TXT))
			configured_tls_connect_mode = ZBX_TCP_SEC_TLS_PSK;
		else
		{
			if (0 != (program_type & (ZBX_PROGRAM_TYPE_GET | ZBX_PROGRAM_TYPE_SENDER)))
				zabbix_log(LOG_LEVEL_CRIT, "invalid value of \"--tls-connect\" parameter");
			else
				zabbix_log(LOG_LEVEL_CRIT, "invalid value of \"TLSConnect\" parameter");

			goto out;
		}
	}

	/* parse and validate 'TLSAccept' parameter used in zabbix_proxy.conf, zabbix_agentd.conf, zabbix_agent.conf */

	zbx_parameter_not_empty(CONFIG_TLS_ACCEPT, "TLSAccept");

	if (NULL != CONFIG_TLS_ACCEPT)
	{
		char		*s, *p, *delim;
		unsigned int	accept_modes_tmp = 0;	/* 'configured_tls_accept_modes' is shared between threads on */
							/* MS Windows. To avoid races make a local temporary */
							/* variable, modify it and write into */
							/* 'configured_tls_accept_modes' when done. */

		p = s = zbx_strdup(NULL, CONFIG_TLS_ACCEPT);

		while (1)
		{
			delim = (NULL == p ? NULL : strchr(p, ','));
			if (NULL != delim)
				*delim = '\0';

			if (0 == strcmp(p, ZBX_TCP_SEC_UNENCRYPTED_TXT))
				accept_modes_tmp |= ZBX_TCP_SEC_UNENCRYPTED;
			else if (0 == strcmp(p, ZBX_TCP_SEC_TLS_CERT_TXT))
				accept_modes_tmp |= ZBX_TCP_SEC_TLS_CERT;
			else if (0 == strcmp(p, ZBX_TCP_SEC_TLS_PSK_TXT))
				accept_modes_tmp |= ZBX_TCP_SEC_TLS_PSK;
			else
			{
				zabbix_log(LOG_LEVEL_CRIT, "invalid value of \"TLSAccept\" parameter");
				zbx_free(s);
				goto out;
			}

			if (NULL == p || NULL == delim)
				break;

			*delim = ',';
			p = delim + 1;
		}

		configured_tls_accept_modes = accept_modes_tmp;

		zbx_free(s);
	}

	/* either both a certificate and a private key must be defined or none of them */

	if (0 != (program_type & (ZBX_PROGRAM_TYPE_GET | ZBX_PROGRAM_TYPE_SENDER)))
	{
		zbx_parameter_not_empty(CONFIG_TLS_CA_FILE, "--tls-ca-file");
		zbx_parameter_not_empty(CONFIG_TLS_CRL_FILE, "--tls-crl-file");
		zbx_parameter_not_empty(CONFIG_TLS_CERT_FILE, "--tls-cert-file");
		zbx_parameter_not_empty(CONFIG_TLS_KEY_FILE, "--tls-key-file");
	}
	else
	{
		zbx_parameter_not_empty(CONFIG_TLS_CA_FILE, "TLSCaFile");
		zbx_parameter_not_empty(CONFIG_TLS_CRL_FILE, "TLSCrlFile");
		zbx_parameter_not_empty(CONFIG_TLS_CERT_FILE, "TLSCertFile");
		zbx_parameter_not_empty(CONFIG_TLS_KEY_FILE, "TLSKeyFile");
	}

	if (NULL != CONFIG_TLS_CERT_FILE && NULL == CONFIG_TLS_KEY_FILE)
	{
		if (0 != (program_type & (ZBX_PROGRAM_TYPE_GET | ZBX_PROGRAM_TYPE_SENDER)))
		{
			zabbix_log(LOG_LEVEL_CRIT, "parameter \"--tls-cert-file\" is defined but "
					"\"--tls-key-file\" is not defined");
		}
		else
		{
			zabbix_log(LOG_LEVEL_CRIT, "configuration parameter \"TLSCertFile\" is defined but "
					"\"TLSKeyFile\" is not defined");
		}
		goto out;
	}

	if (NULL != CONFIG_TLS_KEY_FILE && NULL == CONFIG_TLS_CERT_FILE)
	{
		if (0 != (program_type & (ZBX_PROGRAM_TYPE_GET | ZBX_PROGRAM_TYPE_SENDER)))
		{
			zabbix_log(LOG_LEVEL_CRIT, "parameter \"--tls-key-file\" is defined but "
					"\"--tls-cert-file\" is not defined");
		}
		else
		{
			zabbix_log(LOG_LEVEL_CRIT, "configuration parameter \"TLSKeyFile\" is defined but "
					"\"TLSCertFile\" is not defined");
		}
		goto out;
	}

	/* CA file must be defined only together with a certificate */

	if (NULL != CONFIG_TLS_CERT_FILE && NULL == CONFIG_TLS_CA_FILE)
	{
		if (0 != (program_type & (ZBX_PROGRAM_TYPE_GET | ZBX_PROGRAM_TYPE_SENDER)))
		{
			zabbix_log(LOG_LEVEL_CRIT, "parameter \"--tls-cert-file\" is defined but "
					"\"--tls-ca-file\" is not defined");
		}
		else
		{
			zabbix_log(LOG_LEVEL_CRIT, "configuration parameter \"TLSCertFile\" is defined but "
					"\"TLSCaFile\" is not defined");
		}
		goto out;
	}

	if (NULL != CONFIG_TLS_CA_FILE && NULL == CONFIG_TLS_CERT_FILE)
	{
		if (0 != (program_type & (ZBX_PROGRAM_TYPE_GET | ZBX_PROGRAM_TYPE_SENDER)))
		{
			zabbix_log(LOG_LEVEL_CRIT, "parameter \"--tls-ca-file\" is defined but "
					"\"--tls-cert-file\" and \"--tls-key-file\" are not defined");
		}
		else
		{
			zabbix_log(LOG_LEVEL_CRIT, "configuration parameter \"TLSCaFile\" is defined but "
					"\"TLSCertFile\" and \"TLSKeyFile\" are not defined");
		}
		goto out;
	}

	/* CRL file is optional but must be defined only together with a certificate */

	if (NULL == CONFIG_TLS_CERT_FILE && NULL != CONFIG_TLS_CRL_FILE)
	{
		if (0 != (program_type & (ZBX_PROGRAM_TYPE_GET | ZBX_PROGRAM_TYPE_SENDER)))
		{
			zabbix_log(LOG_LEVEL_CRIT, "parameter \"--tls-crl-file\" is defined but "
					"\"--tls-cert-file\" and \"--tls-key-file\" are not defined");
		}
		else
		{
			zabbix_log(LOG_LEVEL_CRIT, "configuration parameter \"TLSCrlFile\" is defined but "
					"\"TLSCertFile\" and \"TLSKeyFile\" are not defined");
		}
		goto out;
	}

	/* either both a PSK and a PSK-identity must be defined or none of them */

	if (0 != (program_type & (ZBX_PROGRAM_TYPE_GET | ZBX_PROGRAM_TYPE_SENDER)))
	{
		zbx_parameter_not_empty(CONFIG_TLS_PSK_FILE, "--tls-psk-file");
		zbx_parameter_not_empty(CONFIG_TLS_PSK_IDENTITY, "--tls-psk-identity");
	}
	else
	{
		zbx_parameter_not_empty(CONFIG_TLS_PSK_FILE, "TLSPskFile");
		zbx_parameter_not_empty(CONFIG_TLS_PSK_IDENTITY, "TLSPskIdentity");
	}

	if (NULL != CONFIG_TLS_PSK_FILE && NULL == CONFIG_TLS_PSK_IDENTITY)
	{
		if (0 != (program_type & (ZBX_PROGRAM_TYPE_GET | ZBX_PROGRAM_TYPE_SENDER)))
		{
			zabbix_log(LOG_LEVEL_CRIT, "parameter \"--tls-psk-file\" is defined but "
					"\"--tls-psk-identity\" is not defined");
		}
		else
		{
			zabbix_log(LOG_LEVEL_CRIT, "configuration parameter \"TLSPskFile\" is defined but "
					"\"TLSPskIdentity\" is not defined");
		}
		goto out;
	}

	if (NULL != CONFIG_TLS_PSK_IDENTITY && NULL == CONFIG_TLS_PSK_FILE)
	{
		if (0 != (program_type & (ZBX_PROGRAM_TYPE_GET | ZBX_PROGRAM_TYPE_SENDER)))
		{
			zabbix_log(LOG_LEVEL_CRIT, "parameter \"--tls-psk-identity\" is defined but "
					"\"--tls-psk-file\" is not defined");
		}
		else
		{
			zabbix_log(LOG_LEVEL_CRIT, "configuration parameter \"TLSPskIdentity\" is defined but "
					"\"TLSPskFile\" is not defined");
		}
		goto out;
	}

	/* PSK identity must be a valid UTF-8 string (RFC4279 says Unicode) */
	if (NULL != CONFIG_TLS_PSK_IDENTITY && SUCCEED != zbx_is_utf8(CONFIG_TLS_PSK_IDENTITY))
	{
		if (0 != (program_type & (ZBX_PROGRAM_TYPE_GET | ZBX_PROGRAM_TYPE_SENDER)))
		{
			zabbix_log(LOG_LEVEL_CRIT, "parameter \"--tls-psk-identity\" value is not a valid "
					"UTF-8 string");
		}
		else
		{
			zabbix_log(LOG_LEVEL_CRIT, "configuration parameter \"TLSPskIdentity\" value is not a valid "
					"UTF-8 string");
		}
		goto out;
	}

	/* agentd and active proxy specific validation */

	if (0 != (program_type & (ZBX_PROGRAM_TYPE_AGENTD | ZBX_PROGRAM_TYPE_PROXY_ACTIVE)))
	{
		/* 'TLSConnect' is the master parameter to be matched by certificate and PSK parameters. */
		/* 'TLSConnect' will be silently ignored on agentd, if active checks are not configured */
		/* (i.e. 'ServerActive' is not specified. */

		if ((NULL != CONFIG_TLS_CERT_FILE || NULL != CONFIG_TLS_PSK_FILE) && NULL == CONFIG_TLS_CONNECT)
		{
			zabbix_log(LOG_LEVEL_CRIT, "certificate or pre-shared key (PSK) is configured but parameter "
					"\"TLSConnect\" is not defined");
			goto out;
		}

		if (0 != (configured_tls_connect_mode & ZBX_TCP_SEC_TLS_CERT) && NULL == CONFIG_TLS_CERT_FILE)
		{
			zabbix_log(LOG_LEVEL_CRIT, "parameter \"TLSConnect\" value requires a certificate but it is not"
					" configured");
			goto out;
		}

		if (0 != (configured_tls_connect_mode & ZBX_TCP_SEC_TLS_PSK) && NULL == CONFIG_TLS_PSK_FILE)
		{
			zabbix_log(LOG_LEVEL_CRIT, "parameter \"TLSConnect\" value requires a pre-shared key (PSK) but "
					"it is not configured");
			goto out;
		}
	}

	/* agentd, agent and passive proxy specific validation */

	if (0 != (program_type & (ZBX_PROGRAM_TYPE_AGENTD | ZBX_PROGRAM_TYPE_PROXY_PASSIVE | ZBX_PROGRAM_TYPE_AGENT)))
	{
		/* 'TLSAccept' is the master parameter to be matched by certificate and PSK parameters */

		if ((NULL != CONFIG_TLS_CERT_FILE || NULL != CONFIG_TLS_PSK_FILE) && NULL == CONFIG_TLS_ACCEPT)
		{
			zabbix_log(LOG_LEVEL_CRIT, "certificate or pre-shared key (PSK) is configured but parameter "
					"\"TLSAccept\" is not defined");
			goto out;
		}

		if (0 != (configured_tls_accept_modes & ZBX_TCP_SEC_TLS_CERT) && NULL == CONFIG_TLS_CERT_FILE)
		{
			zabbix_log(LOG_LEVEL_CRIT, "parameter \"TLSAccept\" value requires a certificate but it is not"
					" configured");
			goto out;
		}

		if (0 != (configured_tls_accept_modes & ZBX_TCP_SEC_TLS_PSK) && NULL == CONFIG_TLS_PSK_FILE)
		{
			zabbix_log(LOG_LEVEL_CRIT, "parameter \"TLSAccept\" value requires a pre-shared key (PSK) but "
					"it is not configured");
			goto out;
		}
	}

	return;
out:
	zbx_tls_free();
	exit(EXIT_FAILURE);
}
#endif	/* defined(HAVE_POLARSSL) || defined(HAVE_GNUTLS) || defined(HAVE_OPENSSL) */

#if defined(HAVE_POLARSSL)
/******************************************************************************
 *                                                                            *
 * Function: zbx_is_ciphersuite_cert                                          *
 *                                                                            *
 * Purpose: does the specified ciphersuite ID refer to a non-PSK              *
 *          (i.e. certificate) ciphersuite supported for the specified TLS    *
 *          version range                                                     *
 *                                                                            *
 * Comments:                                                                  *
 *          RFC 7465 "Prohibiting RC4 Cipher Suites" requires that RC4 should *
 *          never be used. Also, discard weak encryptions.                    *
 *                                                                            *
 ******************************************************************************/
static int	zbx_is_ciphersuite_cert(const int *p)
{
	const ssl_ciphersuite_t	*info;

	/* PolarSSL function ssl_ciphersuite_uses_psk() is not used here because it can be unavailable in some */
	/* installations. */
	if (NULL != (info = ssl_ciphersuite_from_id(*p)) && POLARSSL_KEY_EXCHANGE_PSK != info->key_exchange &&
			POLARSSL_KEY_EXCHANGE_DHE_PSK != info->key_exchange &&
			POLARSSL_KEY_EXCHANGE_ECDHE_PSK != info->key_exchange &&
			POLARSSL_KEY_EXCHANGE_RSA_PSK != info->key_exchange &&
			POLARSSL_CIPHER_ARC4_128 != info->cipher && 0 == (info->flags & POLARSSL_CIPHERSUITE_WEAK) &&
			(ZBX_TLS_MIN_MAJOR_VER > info->min_major_ver || (ZBX_TLS_MIN_MAJOR_VER == info->min_major_ver &&
			ZBX_TLS_MIN_MINOR_VER >= info->min_minor_ver)) &&
			(ZBX_TLS_MAX_MAJOR_VER < info->max_major_ver || (ZBX_TLS_MAX_MAJOR_VER == info->max_major_ver &&
			ZBX_TLS_MAX_MINOR_VER <= info->max_minor_ver)))
	{
		return SUCCEED;
	}
	else
		return FAIL;
}
#endif

#if defined(HAVE_POLARSSL)
/******************************************************************************
 *                                                                            *
 * Function: zbx_is_ciphersuite_psk                                           *
 *                                                                            *
 * Purpose: does the specified ciphersuite ID refer to a PSK ciphersuite      *
 *          supported for the specified TLS version range                     *
 *                                                                            *
 * Comments:                                                                  *
 *          RFC 7465 "Prohibiting RC4 Cipher Suites" requires that RC4 should *
 *          never be used. Also, discard weak encryptions.                    *
 *                                                                            *
 ******************************************************************************/
static int	zbx_is_ciphersuite_psk(const int *p)
{
	const ssl_ciphersuite_t	*info;

	if (NULL != (info = ssl_ciphersuite_from_id(*p)) && (POLARSSL_KEY_EXCHANGE_PSK == info->key_exchange ||
			POLARSSL_KEY_EXCHANGE_DHE_PSK == info->key_exchange ||
			POLARSSL_KEY_EXCHANGE_ECDHE_PSK == info->key_exchange ||
			POLARSSL_KEY_EXCHANGE_RSA_PSK == info->key_exchange) &&
			POLARSSL_CIPHER_ARC4_128 != info->cipher && 0 == (info->flags & POLARSSL_CIPHERSUITE_WEAK) &&
			(ZBX_TLS_MIN_MAJOR_VER > info->min_major_ver || (ZBX_TLS_MIN_MAJOR_VER == info->min_major_ver &&
			ZBX_TLS_MIN_MINOR_VER >= info->min_minor_ver)) &&
			(ZBX_TLS_MAX_MAJOR_VER < info->max_major_ver || (ZBX_TLS_MAX_MAJOR_VER == info->max_major_ver &&
			ZBX_TLS_MAX_MINOR_VER <= info->max_minor_ver)))
	{
		return SUCCEED;
	}
	else
		return FAIL;
}
#endif

#if defined(HAVE_POLARSSL)
/******************************************************************************
 *                                                                            *
 * Function: zbx_is_ciphersuite_all                                           *
 *                                                                            *
 * Purpose: does the specified ciphersuite ID refer to a good ciphersuite     *
 *          supported for the specified TLS version range                     *
 *                                                                            *
 * Comments:                                                                  *
 *          RFC 7465 "Prohibiting RC4 Cipher Suites" requires that RC4 should *
 *          never be used. Also, discard weak encryptions.                    *
 *                                                                            *
 ******************************************************************************/
static int	zbx_is_ciphersuite_all(const int *p)
{
	const ssl_ciphersuite_t	*info;

	if (NULL != (info = ssl_ciphersuite_from_id(*p)) &&
			POLARSSL_CIPHER_ARC4_128 != info->cipher && 0 == (info->flags & POLARSSL_CIPHERSUITE_WEAK) &&
			(ZBX_TLS_MIN_MAJOR_VER > info->min_major_ver || (ZBX_TLS_MIN_MAJOR_VER == info->min_major_ver &&
			ZBX_TLS_MIN_MINOR_VER >= info->min_minor_ver)) &&
			(ZBX_TLS_MAX_MAJOR_VER < info->max_major_ver || (ZBX_TLS_MAX_MAJOR_VER == info->max_major_ver &&
			ZBX_TLS_MAX_MINOR_VER <= info->max_minor_ver)))
	{
		return SUCCEED;
	}
	else
		return FAIL;
}
#endif

#if defined(HAVE_POLARSSL)
/******************************************************************************
 *                                                                            *
 * Function: zbx_ciphersuites                                                 *
 *                                                                            *
 * Purpose: copy a list of ciphersuites (certificate- or PSK-related) from a  *
 *          list of all supported ciphersuites                                *
 *                                                                            *
 ******************************************************************************/
static unsigned int	zbx_ciphersuites(int type, int **suites)
{
	const int		*supported_suites, *p;
	int			*q;
	unsigned int		count = 0;

	supported_suites = ssl_list_ciphersuites();

	/* count available relevant ciphersuites */
	for (p = supported_suites; *p != 0; p++)
	{
		if (ZBX_TLS_CIPHERSUITE_CERT == type)
		{
			if (SUCCEED != zbx_is_ciphersuite_cert(p))
				continue;
		}
		else if (ZBX_TLS_CIPHERSUITE_PSK == type)
		{
			if (SUCCEED != zbx_is_ciphersuite_psk(p))
				continue;
		}
		else	/* ZBX_TLS_CIPHERSUITE_ALL */
		{
			if (SUCCEED != zbx_is_ciphersuite_all(p))
				continue;
		}

		count++;
	}

	*suites = zbx_malloc(*suites, (count + 1) * sizeof(int));

	/* copy the ciphersuites into array */
	for (p = supported_suites, q = *suites; *p != 0; p++)
	{
		if (ZBX_TLS_CIPHERSUITE_CERT == type)
		{
			if (SUCCEED != zbx_is_ciphersuite_cert(p))
				continue;
		}
		else if (ZBX_TLS_CIPHERSUITE_PSK == type)
		{
			if (SUCCEED != zbx_is_ciphersuite_psk(p))
				continue;
		}
		else	/* ZBX_TLS_CIPHERSUITE_ALL */
		{
			if (SUCCEED != zbx_is_ciphersuite_all(p))
				continue;
		}

		*q++ = *p;
	}

	*q = 0;

	return count;
}
#endif

#if defined(HAVE_POLARSSL) || defined(HAVE_GNUTLS) || defined(HAVE_OPENSSL)
/******************************************************************************
 *                                                                            *
 * Function: zbx_psk_hex2bin                                                  *
 *                                                                            *
 * Purpose:                                                                   *
 *    convert a pre-shared key from a textual representation (ASCII hex digit *
 *    string) to a binary representation (byte string)                        *
 *                                                                            *
 * Parameters:                                                                *
 *     p_hex   - [IN] null-terminated input PSK hex-string                    *
 *     buf     - [OUT] output buffer                                          *
 *     buf_len - [IN] output buffer size                                      *
 *                                                                            *
 * Return value:                                                              *
 *     Number of PSK bytes written into ' buf' on successful conversion.      *
 *     -1 - an error occurred.                                                *
 *                                                                            *
 * Comments:                                                                  *
 *     In case of error incomplete useless data may be written into 'buf'.    *
 *                                                                            *
 ******************************************************************************/
static int	zbx_psk_hex2bin(const unsigned char *p_hex, unsigned char *buf, int buf_len)
{
	unsigned char		*q, hi, lo;
	int			len = 0;

	q = buf;

	while ('\0' != *p_hex)
	{
		if (0 != isxdigit(*p_hex) && 0 != isxdigit(*(p_hex + 1)) && buf_len > len)
		{
			hi = *p_hex & 0x0f;

			if ('9' < *p_hex++)
				hi += 9u;

			lo = *p_hex & 0x0f;

			if ('9' < *p_hex++)
				lo += 9u;

			*q++ = hi << 4 | lo;
			len++;
		}
		else
			return -1;
	}

	return len;
}
#endif

#if defined(HAVE_POLARSSL)
/******************************************************************************
 *                                                                            *
 * Function: zbx_psk_callback                                                 *
 *                                                                            *
 * Purpose:                                                                   *
 *    find and set the requested pre-shared key upon PolarSSL request         *
 *                                                                            *
 * Parameters:                                                                *
 *     par              - [IN] not used                                       *
 *     tls_ctx          - [IN] TLS connection context                         *
 *     psk_identity     - [IN] PSK identity for which the PSK should be       *
 *                             searched and set                               *
 *     psk_identity_len - [IN] size of 'psk_identity'                         *
 *                                                                            *
 * Return value:                                                              *
 *      0  - required PSK successfully found and set                          *
 *     -1 - an error occurred.                                                *
 *                                                                            *
 * Comments:                                                                  *
 *     A callback function, its arguments are defined in PolarSSL.            *
 *     Used only in server and proxy.                                         *
 *                                                                            *
 ******************************************************************************/
static int	zbx_psk_callback(void *par, ssl_context *tls_ctx, const unsigned char *psk_identity,
		size_t psk_identity_len)
{
	const char	*__function_name = "zbx_psk_callback";
	unsigned char	*psk;
	size_t		psk_len = 0;
	int		psk_bin_len;
	unsigned char	tls_psk_identity[HOST_TLS_PSK_IDENTITY_LEN_MAX], tls_psk_hex[HOST_TLS_PSK_LEN_MAX],
			psk_buf[HOST_TLS_PSK_LEN/2];

	if (SUCCEED == zabbix_check_log_level(LOG_LEVEL_DEBUG))
	{
		/* special print: psk_identity is not '\0'-terminated */
		zabbix_log(LOG_LEVEL_DEBUG, "%s(): requested PSK-identity: \"%.*s\"", __function_name, psk_identity_len,
				psk_identity);
	}

	/* try PSK from configuration file first (it is already in binary form) */

	if (0 < my_psk_identity_len && my_psk_identity_len == psk_identity_len &&
			0 == memcmp(my_psk_identity, psk_identity, psk_identity_len))
	{
		psk = (unsigned char *)my_psk;
		psk_len = my_psk_len;
	}
	else	/* search the required PSK in configuration cache */
	{
		if (HOST_TLS_PSK_IDENTITY_LEN < psk_identity_len)
		{
			THIS_SHOULD_NEVER_HAPPEN;
			return -1;
		}

		memcpy(tls_psk_identity, psk_identity, psk_identity_len);
		tls_psk_identity[psk_identity_len] = '\0';

		/* call the function DCget_psk_by_identity() by pointer */
		if (0 < find_psk_in_cache(tls_psk_identity, tls_psk_hex, HOST_TLS_PSK_LEN_MAX))
		{
			/* convert PSK to binary form */
			if (0 >= (psk_bin_len = zbx_psk_hex2bin(tls_psk_hex, psk_buf, sizeof(psk_buf))))
			{
				/* this should have been prevented by validation in frontend or API */
				zabbix_log(LOG_LEVEL_WARNING, "cannot convert PSK to binary form for PSK identity "
						"\"%.*s\"", psk_identity_len, psk_identity);
				return -1;
			}

			psk = psk_buf;
			psk_len = (size_t)psk_bin_len;
		}
		else
		{
			if (SUCCEED == zabbix_check_log_level(LOG_LEVEL_DEBUG))
			{
				zabbix_log(LOG_LEVEL_DEBUG, "%s(): cannot find requested PSK-identity: \"%.*s\"",
						__function_name, psk_identity_len, psk_identity);
			}
		}
	}

	if (0 < psk_len)
	{
		int 	res;

		if (0 == (res = ssl_set_psk(tls_ctx, psk, psk_len, psk_identity, psk_identity_len)))
		{
			return 0;
		}
		else
		{
			zbx_tls_error_msg(res, "", &err_msg);
			zabbix_log(LOG_LEVEL_WARNING, "cannot set PSK for PSK identity \"%.*s\": %s", psk_identity_len,
					psk_identity, err_msg);
			zbx_free(err_msg);
		}
	}

	return -1;
}
#elif defined(HAVE_GNUTLS)
/******************************************************************************
 *                                                                            *
 * Function: zbx_psk_callback                                                 *
 *                                                                            *
 * Purpose:                                                                   *
 *    find and set the requested pre-shared key upon GnuTLS request           *
 *                                                                            *
 * Parameters:                                                                *
 *     session      - [IN] not used                                           *
 *     psk_identity - [IN] PSK identity for which the PSK should be searched  *
 *                         and set                                            *
 *     key          - [OUT pre-shared key allocated and set                   *
 *                                                                            *
 * Return value:                                                              *
 *      0  - required PSK successfully found and set                          *
 *     -1 - an error occurred.                                                *
 *                                                                            *
 * Comments:                                                                  *
 *     A callback function, its arguments are defined in GnuTLS.              *
 *     Used in all programs accepting connections.                            *
 *                                                                            *
 ******************************************************************************/
static int	zbx_psk_callback(gnutls_session_t session, const char *psk_identity, gnutls_datum_t *key)
{
	const char	*__function_name = "zbx_psk_callback";
	char		*psk;
	size_t		psk_len = 0;
	int		psk_bin_len;
	unsigned char	tls_psk_hex[HOST_TLS_PSK_LEN_MAX], psk_buf[HOST_TLS_PSK_LEN/2];

	if (SUCCEED == zabbix_check_log_level(LOG_LEVEL_DEBUG))
		zabbix_log(LOG_LEVEL_DEBUG, "%s(): requested PSK-identity: \"%s\"", __function_name, psk_identity);

	/* try PSK from configuration file first (it is already in binary form) */

	if (0 < my_psk_identity_len && 0 == strcmp(my_psk_identity, psk_identity))
	{
		psk = my_psk;
		psk_len = my_psk_len;
	}
	else if (0 != (program_type & (ZBX_PROGRAM_TYPE_PROXY | ZBX_PROGRAM_TYPE_SERVER)))
	{
		/* search the required PSK in configuration cache */

		/* call the function DCget_psk_by_identity() by pointer */
		if (0 < find_psk_in_cache((const unsigned char *)psk_identity, tls_psk_hex, HOST_TLS_PSK_LEN_MAX))
		{
			/* convert PSK to binary form */
			if (0 >= (psk_bin_len = zbx_psk_hex2bin(tls_psk_hex, psk_buf, sizeof(psk_buf))))
			{
				/* this should have been prevented by validation in frontend or API */
				zabbix_log(LOG_LEVEL_WARNING, "cannot convert PSK to binary form for PSK identity "
						"\"%s\"", psk_identity);
				return -1;	/* fail */
			}

			psk = (char *)psk_buf;
			psk_len = (size_t)psk_bin_len;
		}
		else
		{
			if (SUCCEED == zabbix_check_log_level(LOG_LEVEL_DEBUG))
			{
				zabbix_log(LOG_LEVEL_DEBUG, "%s(): cannot find requested PSK-identity: \"%s\"",
						__function_name, psk_identity);
			}
		}
	}

	if (0 < psk_len)
	{
		if (NULL == (key->data = gnutls_malloc(psk_len)))
		{
			zabbix_log(LOG_LEVEL_WARNING, "cannot allocate %zu bytes of memory for PSK with identity "
					"\"%s\"", psk_len, psk_identity);
			return -1;	/* fail */
		}

		memcpy(key->data, psk, psk_len);
		key->size = (unsigned int)psk_len;

		return 0;	/* success */
	}

	return -1;	/* PSK not found */
}
#elif defined(HAVE_OPENSSL)
/******************************************************************************
 *                                                                            *
 * Function: zbx_psk_client_callback                                          *
 *                                                                            *
 * Purpose:                                                                   *
 *    set pre-shared key for outgoing TLS connection upon OpenSSL request     *
 *                                                                            *
 * Parameters:                                                                *
 *     ssl              - [IN] not used                                       *
 *     hint             - [IN] not used                                       *
 *     identity         - [OUT] buffer to write PSK identity into             *
 *     max_identity_len - [IN] size of the 'identity' buffer                  *
 *     psk              - [OUT] buffer to write PSK into                      *
 *     max_psk_len      - [IN] size of the 'psk' buffer                       *
 *                                                                            *
 * Return value:                                                              *
 *     > 0 - length of PSK in bytes                                           *
 *       0 - an error occurred.                                               *
 *                                                                            *
 * Comments:                                                                  *
 *     A callback function, its arguments are defined in OpenSSL.             *
 *     Used in all programs making outgoing TLS PSK connections.              *
 *                                                                            *
 *     As a client we use different PSKs depending on connection to be made.  *
 *     Apparently there is no simple way to specify which PSK should be set   *
 *     by this callback function. We use global variables to pass this info.  *
 *                                                                            *
 ******************************************************************************/
static unsigned int	zbx_psk_client_callback(SSL *ssl, const char *hint, char *identity,
		unsigned int max_identity_len, unsigned char *psk, unsigned int max_psk_len)
{
	const char	*__function_name = "zbx_psk_client_callback";

	if (SUCCEED == zabbix_check_log_level(LOG_LEVEL_DEBUG))
	{
		zabbix_log(LOG_LEVEL_DEBUG, "%s(): requested PSK-identity: \"%s\"", __function_name,
				psk_identity_for_cb);
	}

	if (max_identity_len < psk_identity_len_for_cb + 1)	/* 1 byte for terminating '\0' */
	{
		zabbix_log(LOG_LEVEL_WARNING, "requested PSK-identity \"%s\" does not fit into %u-byte buffer",
				psk_identity_for_cb, max_identity_len);
		return 0;
	}

	if (max_psk_len < psk_len_for_cb)
	{
		zabbix_log(LOG_LEVEL_WARNING, "PSK key associated with PSK-identity \"%s\" does not fit into %u-byte "
				"buffer", psk_identity_for_cb, max_psk_len);
		return 0;
	}

	zbx_strlcpy(identity, psk_identity_for_cb, max_identity_len);
	memcpy(psk, psk_for_cb, psk_len_for_cb);

	return (unsigned int)psk_len_for_cb;
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_psk_server_callback                                          *
 *                                                                            *
 * Purpose:                                                                   *
 *    set pre-shared key for incoming TLS connection upon OpenSSL request     *
 *                                                                            *
 * Parameters:                                                                *
 *     ssl              - [IN] not used                                       *
 *     identity         - [IN] PSK identity sent by client                    *
 *     psk              - [OUT] buffer to write PSK into                      *
 *     max_psk_len      - [IN] size of the 'psk' buffer                       *
 *                                                                            *
 * Return value:                                                              *
 *     > 0 - length of PSK in bytes                                           *
 *       0 - PSK identity not found                                           *
 *                                                                            *
 * Comments:                                                                  *
 *     A callback function, its arguments are defined in OpenSSL.             *
 *     Used in all programs accepting incoming TLS PSK connections.           *
 *                                                                            *
 ******************************************************************************/
static unsigned int	zbx_psk_server_callback(SSL *ssl, const char *identity, unsigned char *psk,
		unsigned int max_psk_len)
{
	const char	*__function_name = "zbx_psk_server_callback";
	char		*psk_loc;
	size_t		psk_len = 0;
	int		psk_bin_len;
	unsigned char	tls_psk_hex[HOST_TLS_PSK_LEN_MAX], psk_buf[HOST_TLS_PSK_LEN/2];

	if (SUCCEED == zabbix_check_log_level(LOG_LEVEL_DEBUG))
		zabbix_log(LOG_LEVEL_DEBUG, "%s(): requested PSK-identity: \"%s\"", __function_name, identity);

	/* try PSK from configuration file first (it is already in binary form) */

	if (0 < my_psk_identity_len && 0 == strcmp(my_psk_identity, identity))
	{
		psk_loc = my_psk;
		psk_len = my_psk_len;
	}
	else if (0 != (program_type & (ZBX_PROGRAM_TYPE_PROXY | ZBX_PROGRAM_TYPE_SERVER)))
	{
		/* search the required PSK in configuration cache */

		/* call the function DCget_psk_by_identity() by pointer */
		if (0 < find_psk_in_cache((const unsigned char *)identity, tls_psk_hex, HOST_TLS_PSK_LEN_MAX))
		{
			/* convert PSK to binary form */
			if (0 >= (psk_bin_len = zbx_psk_hex2bin(tls_psk_hex, psk_buf, sizeof(psk_buf))))
			{
				/* this should have been prevented by validation in frontend or API */
				zabbix_log(LOG_LEVEL_WARNING, "cannot convert PSK to binary form for PSK identity "
						"\"%s\"", identity);
				return 0;	/* fail */
			}

			psk_loc = (char *)psk_buf;
			psk_len = (size_t)psk_bin_len;
		}
		else
		{
			if (SUCCEED == zabbix_check_log_level(LOG_LEVEL_DEBUG))
			{
				zabbix_log(LOG_LEVEL_DEBUG, "%s(): cannot find requested PSK-identity: \"%s\"",
						__function_name, identity);
			}

			return 0;	/* PSK not found */
		}
	}

	if (0 < psk_len)
	{
		if ((size_t)max_psk_len < psk_len)
		{
			zabbix_log(LOG_LEVEL_WARNING, "PSK key associated with PSK-identity \"%s\" does not fit into "
					"%u-byte buffer", identity, max_psk_len);
			return 0;	/* fail */
		}

		memcpy(psk, psk_loc, psk_len);

		return (unsigned int)psk_len;	/* success */
	}

	return 0;	/* PSK not found */
}
#endif


#if defined(HAVE_POLARSSL) || defined(HAVE_GNUTLS) || defined(HAVE_OPENSSL)
/******************************************************************************
 *                                                                            *
 * Function: zbx_read_psk_file                                                *
 *                                                                            *
 * Purpose:                                                                   *
 *    Read a pre-shared key from a configured file and convert it from        *
 *    textual representation (ASCII hex digit string) to a binary             *
 *    representation (byte string)                                            *
 *                                                                            *
 ******************************************************************************/
static void	zbx_read_psk_file(void)
{
	FILE		*f;
	size_t		len;
	char		*p;
	int		len_bin;
	unsigned int	i;
	zbx_stat_t	stat_buf;
	char		buf[HOST_TLS_PSK_LEN_MAX + 2];	/* up to 512 bytes of hex-digits, maybe 1-2 bytes for '\n', */
							/* 1 byte for terminating '\0' */
	char		buf_bin[HOST_TLS_PSK_LEN/2];	/* up to 256 bytes of binary PSK */

	if (NULL == (f = fopen(CONFIG_TLS_PSK_FILE, "r")))
	{
		zabbix_log(LOG_LEVEL_CRIT, "cannot open file \"%s\": %s", CONFIG_TLS_PSK_FILE, zbx_strerror(errno));
		goto out;
	}

	/* fgets() produces a confusing error message "[2] No such file or directory" if reading from an empty file. */
	/* To avoid the confusing message get the file size and deal with this situation separately. */

	if (0 != zbx_stat(CONFIG_TLS_PSK_FILE, &stat_buf))
	{
		zabbix_log(LOG_LEVEL_CRIT, "cannot obtain information for file \"%s\": %s", CONFIG_TLS_PSK_FILE,
				zbx_strerror(errno));
		goto out;
	}

	if (0 == stat_buf.st_size)
	{
		zabbix_log(LOG_LEVEL_CRIT, "PSK file \"%s\" is empty", CONFIG_TLS_PSK_FILE);
		goto out;
	}

	if (NULL == fgets(buf, (int)sizeof(buf), f))
	{
		zabbix_log(LOG_LEVEL_CRIT, "cannot read from file \"%s\": %s", CONFIG_TLS_PSK_FILE,
				zbx_strerror(errno));
		goto out;
	}

	if (0 != fclose(f))
	{
		zabbix_log(LOG_LEVEL_CRIT, "cannot close file \"%s\": %s", CONFIG_TLS_PSK_FILE, zbx_strerror(errno));
		goto out;
	}

	if (NULL != (p = strchr(buf, '\n')))
		*p = '\0';

	if (0 == (len = strlen(buf)))
	{
		zabbix_log(LOG_LEVEL_CRIT, "file \"%s\" is empty", CONFIG_TLS_PSK_FILE);
		goto out;
	}

#if defined(HAVE_POLARSSL)
	/* Currently PolarSSL supports up to 32 bytes long PSKs, but with other libraries we can support up */
	/* to 256 bytes. Check both limits for safer code. */
	if (POLARSSL_PSK_MAX_LEN * 2 < len || HOST_TLS_PSK_LEN < len)
#elif defined(HAVE_GNUTLS) || defined(HAVE_OPENSSL)
	if (HOST_TLS_PSK_LEN < len)
#endif
	{
		zabbix_log(LOG_LEVEL_CRIT, "PSK in file \"%s\" is too large", CONFIG_TLS_PSK_FILE);
		goto out;
	}

	if (0 >= (len_bin = zbx_psk_hex2bin((unsigned char *)buf, (unsigned char *)buf_bin, sizeof(buf_bin))))
	{
		zabbix_log(LOG_LEVEL_CRIT, "invalid PSK in file \"%s\"", CONFIG_TLS_PSK_FILE);
		goto out;
	}

	my_psk_len = (size_t)len_bin;
	my_psk = zbx_malloc(my_psk, my_psk_len);

	for (i = 0; i < my_psk_len; i++)
		my_psk[i] = buf_bin[i];

	return;
out:
	zbx_tls_free();
	exit(EXIT_FAILURE);
}
#endif

#if defined(HAVE_OPENSSL)
/******************************************************************************
 *                                                                            *
 * Function: zbx_log_ciphersuites                                             *
 *                                                                            *
 * Purpose: write names of enabled OpenSSL ciphersuites into Zabbix log for   *
 *          debugging                                                         *
 *                                                                            *
 ******************************************************************************/
static void	zbx_log_ciphersuites(const char *title, const SSL *ctx)
{
	if (SUCCEED == zabbix_check_log_level(LOG_LEVEL_DEBUG))
	{
		const char	*cipher_name;
		char		*msg = NULL;
		size_t		msg_alloc = 0, msg_offset = 0;
		int		i = 0;

		zbx_snprintf_alloc(&msg, &msg_alloc, &msg_offset, "%s() ciphersuites:", title);

		while (NULL != (cipher_name = SSL_get_cipher_list(ctx, i++)))
			zbx_snprintf_alloc(&msg, &msg_alloc, &msg_offset, " %s", cipher_name);

		zabbix_log(LOG_LEVEL_DEBUG, "%s", msg);
		zbx_free(msg);
	}
}
#endif

/******************************************************************************
 *                                                                            *
 * Function: zbx_tls_init_parent                                              *
 *                                                                            *
 * Purpose: initialize TLS library in a parent process                        *
 *                                                                            *
 ******************************************************************************/
int	zbx_tls_init_parent(void)
{
	const char	*__function_name = "zbx_tls_init_parent";
	int		ret = SUCCEED;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);
	/* TODO fill in implementation */
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __function_name, zbx_result_string(ret));

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_tls_init_child                                               *
 *                                                                            *
 * Purpose: read available configuration parameters and initialize TLS        *
 *          library in a child process                                        *
 *                                                                            *
 ******************************************************************************/
void	zbx_tls_init_child(void)
{
#if defined(HAVE_POLARSSL)
	const char	*__function_name = "zbx_tls_init_child";
	int		res;
	unsigned int	cipher_count;
	char		*pers = NULL;
	size_t		pers_len = 0;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	zbx_tls_validate_config();

	/* 'TLSCaFile' parameter (in zabbix_server.conf, zabbix_proxy.conf, zabbix_agentd.conf, zabbix_agent.conf). */
	if (NULL != CONFIG_TLS_CA_FILE)
	{
		ca_cert = zbx_malloc(ca_cert, sizeof(x509_crt));
		x509_crt_init(ca_cert);

		if (0 != (res = x509_crt_parse_file(ca_cert, CONFIG_TLS_CA_FILE)))
		{
			if (0 > res)
			{
				zbx_tls_error_msg(res, "", &err_msg);
				zabbix_log(LOG_LEVEL_CRIT, "cannot parse CA certificate(s) in file \"%s\": %s",
						CONFIG_TLS_CA_FILE, err_msg);
				zbx_free(err_msg);
			}
			else
			{
				zabbix_log(LOG_LEVEL_CRIT, "cannot parse %d CA certificate(s) in file \"%s\"", res,
						CONFIG_TLS_CA_FILE);
			}

			zbx_tls_free();
			exit(EXIT_FAILURE);
		}

		if (SUCCEED == zabbix_check_log_level(LOG_LEVEL_DEBUG))
		{
			/* x509_crt_info() uses a fixed buffer and there is no way to know how large this buffer */
			/* should be to accumulate all info. */
			if (-1 != x509_crt_info(work_buf, sizeof(work_buf), "", ca_cert))
			{
				zabbix_log(LOG_LEVEL_DEBUG, "%s(): successfully loaded CA certificate(s) from file "
						"(output may be truncated):\n%s", __function_name, work_buf);
			}
			else
			{
				zabbix_log(LOG_LEVEL_DEBUG, "%s(): cannot print CA certificate(s) info",
						__function_name);
				zbx_tls_free();
				exit(EXIT_FAILURE);
			}
		}
	}

	/* 'TLSCrlFile' parameter (in zabbix_server.conf, zabbix_proxy.conf, zabbix_agentd.conf, zabbix_agent.conf). */
	/* Load CRL (certificate revocation list) file. */
	if (NULL != CONFIG_TLS_CRL_FILE)
	{
		crl = zbx_malloc(crl, sizeof(x509_crl));
		x509_crl_init(crl);

		if (0 != (res = x509_crl_parse_file(crl, CONFIG_TLS_CRL_FILE)))
		{
			if (0 > res)
			{
				zbx_tls_error_msg(res, "", &err_msg);
				zabbix_log(LOG_LEVEL_CRIT, "cannot parse CRL file \"%s\": %s", CONFIG_TLS_CRL_FILE,
						err_msg);
				zbx_free(err_msg);
			}
			else
			{
				zabbix_log(LOG_LEVEL_CRIT, "cannot parse %d certificate(s) in CRL file \"%s\"", res,
						CONFIG_TLS_CRL_FILE);
			}

			zbx_tls_free();
			exit(EXIT_FAILURE);
		}

		if (SUCCEED == zabbix_check_log_level(LOG_LEVEL_DEBUG))
		{
			/* x509_crl_info() uses a fixed buffer and there is no way to know how large this buffer */
			/* should be to accumulate all info. */
			if (-1 != x509_crl_info(work_buf, sizeof(work_buf), "", crl))
			{
				zabbix_log(LOG_LEVEL_DEBUG, "%s(): successfully loaded CRL from file (output may be "
						"truncated):\n%s", __function_name, work_buf);
			}
			else
			{
				zabbix_log(LOG_LEVEL_DEBUG, "%s(): cannot print CRL info", __function_name);
				zbx_tls_free();
				exit(EXIT_FAILURE);
			}
		}
	}

	/* 'TLSCertFile' parameter (in zabbix_server.conf, zabbix_proxy.conf, zabbix_agentd.conf, zabbix_agent.conf). */
	/* Load certificate. */
	if (NULL != CONFIG_TLS_CERT_FILE)
	{
		my_cert = zbx_malloc(my_cert, sizeof(x509_crt));
		x509_crt_init(my_cert);

		if (0 != (res = x509_crt_parse_file(my_cert, CONFIG_TLS_CERT_FILE)))
		{
			if (0 > res)
			{
				zbx_tls_error_msg(res, "", &err_msg);
				zabbix_log(LOG_LEVEL_CRIT, "cannot parse certificate(s) in file \"%s\": %s",
						CONFIG_TLS_CERT_FILE, err_msg);
				zbx_free(err_msg);
			}
			else
			{
				zabbix_log(LOG_LEVEL_CRIT, "cannot parse %d certificate(s) in file \"%s\"", res,
						CONFIG_TLS_CERT_FILE);
			}

			zbx_tls_free();
			exit(EXIT_FAILURE);
		}

		if (SUCCEED == zabbix_check_log_level(LOG_LEVEL_DEBUG))
		{
			/* x509_crt_info() uses a fixed buffer and there is no way to know how large this buffer */
			/* should be to accumulate all info. */
			if (-1 != x509_crt_info(work_buf, sizeof(work_buf), "", my_cert))
			{
				zabbix_log(LOG_LEVEL_DEBUG, "%s(): successfully loaded certificate (output may be "
						"truncated):\n%s", __function_name, work_buf);
			}
			else
			{
				zabbix_log(LOG_LEVEL_DEBUG, "%s(): cannot print certificate info", __function_name);
				zbx_tls_free();
				exit(EXIT_FAILURE);
			}
		}
	}

	/* 'TLSKeyFile' parameter (in zabbix_server.conf, zabbix_proxy.conf, zabbix_agentd.conf, zabbix_agent.conf). */
	/* Load private key. */
	if (NULL != CONFIG_TLS_KEY_FILE)
	{
		my_priv_key = zbx_malloc(my_priv_key, sizeof(pk_context));
		pk_init(my_priv_key);

		/* The 3rd argument of pk_parse_keyfile() is password for decrypting the private key. */
		/* Currently the password is not used, it is empty. */
		if (0 != (res = pk_parse_keyfile(my_priv_key, CONFIG_TLS_KEY_FILE, "")))
		{
			zbx_tls_error_msg(res, "", &err_msg);
			zabbix_log(LOG_LEVEL_CRIT, "cannot parse the private key in file \"%s\": %s",
					CONFIG_TLS_KEY_FILE, err_msg);
			zbx_free(err_msg);
			zbx_tls_free();
			exit(EXIT_FAILURE);
		}

		zabbix_log(LOG_LEVEL_DEBUG, "%s(): successfully loaded %zu-bit %s private key", __function_name,
				pk_get_size(my_priv_key), pk_get_name(my_priv_key));
	}

	/* 'TLSPskFile' parameter (in zabbix_server.conf, zabbix_proxy.conf, zabbix_agentd.conf, zabbix_agent.conf). */
	/* Load pre-shared key. */
	if (NULL != CONFIG_TLS_PSK_FILE)
	{
		zbx_read_psk_file();
		zabbix_log(LOG_LEVEL_DEBUG, "%s(): successfully loaded pre-shared key", __function_name);
	}

	/* 'TLSPskIdentity' parameter (in zabbix_proxy.conf, zabbix_agentd.conf, zabbix_agent.conf). Configure */
	/* identity to be used with the pre-shared key. */
	if (NULL != CONFIG_TLS_PSK_IDENTITY)
	{
		my_psk_identity = CONFIG_TLS_PSK_IDENTITY;
		my_psk_identity_len = strlen(my_psk_identity);

		zabbix_log(LOG_LEVEL_DEBUG, "%s(): successfully loaded pre-shared key\'s identity", __function_name);
	}

	/* Certificate always comes from configuration file. Set up ciphersuites. */
	if (NULL != my_cert)
	{
		cipher_count = zbx_ciphersuites(ZBX_TLS_CIPHERSUITE_CERT, &ciphersuites_cert);
		zabbix_log(LOG_LEVEL_DEBUG, "%s(): set up a list of %u certificate ciphersuites", __function_name,
				cipher_count);
	}

	/* PSK can come from configuration file (in proxy, agentd, agent) and later from database (in server, proxy). */
	/* Configure ciphersuites just in case they will be used. */
	if (NULL != my_psk || 0 != (program_type & (ZBX_PROGRAM_TYPE_SERVER | ZBX_PROGRAM_TYPE_PROXY)))
	{
		cipher_count = zbx_ciphersuites(ZBX_TLS_CIPHERSUITE_PSK, &ciphersuites_psk);
		zabbix_log(LOG_LEVEL_DEBUG, "%s(): set up a list of %u PSK ciphersuites", __function_name,
				cipher_count);
	}

	/* Sometimes we need to be ready for both certificate and PSK whichever comes in. Set up a combined list of */
	/* ciphersuites. */
	if (NULL != my_cert && (NULL != my_psk ||
			0 != (program_type & (ZBX_PROGRAM_TYPE_SERVER | ZBX_PROGRAM_TYPE_PROXY))))
	{
		cipher_count = zbx_ciphersuites(ZBX_TLS_CIPHERSUITE_ALL, &ciphersuites_all);
		zabbix_log(LOG_LEVEL_DEBUG, "%s(): set up a list of %u certificate and PSK ciphersuites",
				__function_name, cipher_count);
	}

	entropy = zbx_malloc(entropy, sizeof(entropy_context));
	entropy_init(entropy);

	zbx_tls_make_personalization_string(&pers, &pers_len);

	ctr_drbg = zbx_malloc(ctr_drbg, sizeof(ctr_drbg_context));

	if (0 != (res = ctr_drbg_init(ctr_drbg, entropy_func, entropy, (unsigned char *)pers, pers_len)))
	{
		zbx_tls_error_msg(res, "", &err_msg);
		zabbix_log(LOG_LEVEL_CRIT, "cannot initialize random number generator: %s", err_msg);
		zbx_free(err_msg);
		zbx_tls_free();
		exit(EXIT_FAILURE);
	}

	zbx_guaranteed_memset(pers, 0, pers_len);
	zbx_free(pers);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);

#elif defined(HAVE_GNUTLS)
	const char	*__function_name = "zbx_tls_init_child";
	int		res;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	zbx_tls_validate_config();

	if (GNUTLS_E_SUCCESS != gnutls_global_init())
	{
		zabbix_log(LOG_LEVEL_CRIT, "cannot initialize GnuTLS library");
		exit(EXIT_FAILURE);
	}

	zabbix_log(LOG_LEVEL_DEBUG, "GnuTLS library v.%s initialized", gnutls_check_version(NULL));

	/* need to allocate certificate credentials store ? */

	if (NULL != CONFIG_TLS_CERT_FILE)
	{
		if (0 != (res = gnutls_certificate_allocate_credentials(&my_cert_creds)))
		{
			zabbix_log(LOG_LEVEL_CRIT, "gnutls_certificate_allocate_credentials() failed: %d: %s", res,
					gnutls_strerror(res));
			zbx_tls_free();
			exit(EXIT_FAILURE);
		}
	}

	/* 'TLSCaFile' parameter (in zabbix_server.conf, zabbix_proxy.conf, zabbix_agentd.conf, zabbix_agent.conf) */
	if (NULL != CONFIG_TLS_CA_FILE)
	{
		if (0 < (res = gnutls_certificate_set_x509_trust_file(my_cert_creds, CONFIG_TLS_CA_FILE,
				GNUTLS_X509_FMT_PEM)))
		{
			zabbix_log(LOG_LEVEL_DEBUG, "loaded %d CA certificate(s) from file \"%s\"", res,
					CONFIG_TLS_CA_FILE);
		}
		else if (0 == res)
		{
			zabbix_log(LOG_LEVEL_WARNING, "no CA certificate(s) in file \"%s\"", CONFIG_TLS_CA_FILE);
		}
		else
		{
			zabbix_log(LOG_LEVEL_CRIT, "cannot parse CA certificate(s) in file \"%s\": %d: %s",
				CONFIG_TLS_CA_FILE, res, gnutls_strerror(res));
			zbx_tls_free();
			exit(EXIT_FAILURE);
		}
	}

	/* 'TLSCrlFile' parameter (in zabbix_server.conf, zabbix_proxy.conf, zabbix_agentd.conf, zabbix_agent.conf). */
	/* Load CRL (certificate revocation list) file. */
	if (NULL != CONFIG_TLS_CRL_FILE)
	{
		if (0 < (res = gnutls_certificate_set_x509_crl_file(my_cert_creds, CONFIG_TLS_CRL_FILE,
				GNUTLS_X509_FMT_PEM)))
		{
			zabbix_log(LOG_LEVEL_DEBUG, "loaded %d certificate(s) from CRL file \"%s\"", res,
					CONFIG_TLS_CRL_FILE);
		}
		else if (0 == res)
		{
			zabbix_log(LOG_LEVEL_WARNING, "no certificate(s) in CRL \"%s\"", CONFIG_TLS_CRL_FILE);
		}
		else
		{
			zabbix_log(LOG_LEVEL_CRIT, "cannot parse CRL file \"%s\": %d: %s", CONFIG_TLS_CRL_FILE, res,
					gnutls_strerror(res));
			zbx_tls_free();
			exit(EXIT_FAILURE);
		}
	}

	/* 'TLSCertFile' and 'TLSKeyFile' parameters (in zabbix_server.conf, zabbix_proxy.conf, zabbix_agentd.conf, */
	/* zabbix_agent.conf). Load certificate and private key. */
	if (NULL != CONFIG_TLS_CERT_FILE)
	{
		if (GNUTLS_E_SUCCESS != gnutls_certificate_set_x509_key_file(my_cert_creds, CONFIG_TLS_CERT_FILE,
				CONFIG_TLS_KEY_FILE, GNUTLS_X509_FMT_PEM))
		{
			zabbix_log(LOG_LEVEL_CRIT, "cannot load certificate or private key from file \"%s\" or \"%s\": "
					"%d: %s", CONFIG_TLS_CERT_FILE, CONFIG_TLS_KEY_FILE, res, gnutls_strerror(res));
			zbx_tls_free();
			exit(EXIT_FAILURE);
		}
		else
		{
			zabbix_log(LOG_LEVEL_DEBUG, "%s(): successfully loaded certificate and private key",
					__function_name);
		}
	}

	/* 'TLSPskIdentity' and 'TLSPskFile' parameters used in zabbix_proxy.conf, zabbix_agentd.conf, */
	/* zabbix_agent.conf. Load pre-shared key and identity to be used with the pre-shared key. */

	if (NULL != CONFIG_TLS_PSK_FILE)
	{
		gnutls_datum_t	key;

		my_psk_identity = CONFIG_TLS_PSK_IDENTITY;
		my_psk_identity_len = strlen(my_psk_identity);

		zbx_read_psk_file();

		key.data = (unsigned char *)my_psk;
		key.size = (unsigned int)my_psk_len;

		/* allocate here only PSK credential stores which do not change (e.g. for proxy communication with */
		/* server) */

		if (0 != (program_type & (ZBX_PROGRAM_TYPE_PROXY_ACTIVE | ZBX_PROGRAM_TYPE_AGENTD |
				ZBX_PROGRAM_TYPE_SENDER | ZBX_PROGRAM_TYPE_GET)))
		{
			if (0 != (res = gnutls_psk_allocate_client_credentials(&my_psk_client_creds)))
			{
				zabbix_log(LOG_LEVEL_CRIT, "gnutls_psk_allocate_client_credentials() failed: %d: %s",
						res, gnutls_strerror(res));
				zbx_tls_free();
				exit(EXIT_FAILURE);
			}

			/* TODO docs say 'my_psk_identity' should be prepared with "SASLprep" profile of "stringprep" */
			if (GNUTLS_E_SUCCESS != (res = gnutls_psk_set_client_credentials(my_psk_client_creds,
					my_psk_identity, &key, GNUTLS_PSK_KEY_RAW)))
			{
				zabbix_log(LOG_LEVEL_CRIT, "gnutls_psk_set_client_credentials() failed: %d: %s", res,
						gnutls_strerror(res));
				zbx_tls_free();
				exit(EXIT_FAILURE);
			}
		}

		if (0 != (program_type & (ZBX_PROGRAM_TYPE_PROXY_PASSIVE | ZBX_PROGRAM_TYPE_AGENTD |
				ZBX_PROGRAM_TYPE_AGENT)))
		{
			if (0 != (res = gnutls_psk_allocate_server_credentials(&my_psk_server_creds)))
			{
				zabbix_log(LOG_LEVEL_CRIT, "gnutls_psk_allocate_server_credentials() failed: %d: %s",
						res, gnutls_strerror(res));
				zbx_tls_free();
				exit(EXIT_FAILURE);
			}

			/* Apparently GnuTLS does not provide API for setting up static server credentials (with a */
			/* fixed PSK identity and key) for a passive proxy, agent and agentd. The only possibility */
			/* seems to set up credentials dynamically for each incoming connection using a callback */
			/* function. */
			gnutls_psk_set_server_credentials_function(my_psk_server_creds, zbx_psk_callback);
		}

		zabbix_log(LOG_LEVEL_DEBUG, "%s(): successfully loaded pre-shared key and identity", __function_name);
	}

	/* Certificate always comes from configuration file. Set up ciphersuites. */
	if (NULL != my_cert_creds)
	{
		/* SECURE256 is currently an alias for SECURE192. Use SECURE256 to enable compilation with GnuTLS */
		/* 2.8.x. used on RHEL6. */
		if (GNUTLS_E_SUCCESS != (res = gnutls_priority_init(&ciphersuites_cert, "SECURE192:+SECURE128:"
				"-VERS-TLS-ALL:+VERS-TLS1.2:-CTYPE-ALL:+CTYPE-X.509:-SHA1", NULL)))
		{
			zabbix_log(LOG_LEVEL_CRIT, "gnutls_priority_init() for 'ciphersuites_cert' failed: %d: %s",
					res, gnutls_strerror(res));
			zbx_tls_free();
			exit(EXIT_FAILURE);
		}
	}

	/* PSK can come from configuration file (in proxy, agentd, agent) and later from database (in server, proxy). */
	/* Configure ciphersuites just in case they will be used. */
	if (NULL != my_psk_client_creds || NULL != my_psk_server_creds ||
			0 != (program_type & (ZBX_PROGRAM_TYPE_SERVER | ZBX_PROGRAM_TYPE_PROXY)))
	{
		if (GNUTLS_E_SUCCESS != (res = gnutls_priority_init(&ciphersuites_psk, "SECURE192:+SECURE128:"
				"-VERS-TLS-ALL:+VERS-TLS1.2:-CTYPE-ALL:-ECDHE-ECDSA:-ECDHE-RSA:-RSA:-DHE-RSA:-DHE-DSS:"
				"+DHE-PSK:+ECDHE-PSK:+RSA-PSK:+PSK:-SHA1", NULL)))
		{
			zabbix_log(LOG_LEVEL_CRIT, "gnutls_priority_init() for 'ciphersuites_psk' failed: %d: %s",
					res, gnutls_strerror(res));
			zbx_tls_free();
			exit(EXIT_FAILURE);
		}
	}

	/* Sometimes we need to be ready for both certificate and PSK whichever comes in. Set up a combined list of */
	/* ciphersuites. */
	if (NULL != my_cert_creds && (NULL != my_psk_client_creds || NULL != my_psk_server_creds ||
			0 != (program_type & (ZBX_PROGRAM_TYPE_SERVER | ZBX_PROGRAM_TYPE_PROXY))))
	{
		if (GNUTLS_E_SUCCESS != (res = gnutls_priority_init(&ciphersuites_all, "SECURE192:+SECURE128:"
				"-VERS-TLS-ALL:+VERS-TLS1.2:-CTYPE-ALL:+CTYPE-X.509:+DHE-PSK:+ECDHE-PSK:+RSA-PSK:+PSK:"
				"-SHA1" , NULL)))
		{
			zabbix_log(LOG_LEVEL_CRIT, "gnutls_priority_init() for 'ciphersuites_all' failed: %d: %s",
					res, gnutls_strerror(res));
			zbx_tls_free();
			exit(EXIT_FAILURE);
		}
	}

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);

#elif defined(HAVE_OPENSSL)

	const char	*__function_name = "zbx_tls_init_child";
	char		*error = NULL;
	size_t		error_alloc = 0, error_offset = 0;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	zbx_tls_validate_config();

	SSL_load_error_strings();
	ERR_load_BIO_strings();
	SSL_library_init();		/* always returns "1" */

	/* TODO PRNG initialization */

	/* set protocol version to TLS 1.2 */

	if (0 != (program_type & (ZBX_PROGRAM_TYPE_SENDER | ZBX_PROGRAM_TYPE_GET)))
		method = TLSv1_2_client_method();
	else if (0 != (program_type & ZBX_PROGRAM_TYPE_AGENT))
		method = TLSv1_2_server_method();
	else	/* ZBX_PROGRAM_TYPE_SERVER | ZBX_PROGRAM_TYPE_PROXY | ZBX_PROGRAM_TYPE_AGENTD */
		method = TLSv1_2_method();

	/* create context for certificate-only authentication if certificate is configured */
	if (NULL != CONFIG_TLS_CERT_FILE && NULL == (ctx_cert = SSL_CTX_new(method)))
		goto out_method;

	/* Create context for PSK-only authentication. PSK can come from configuration file (in proxy, agentd, agent) */
	/* and later from database (in server, proxy). */
	if ((NULL != CONFIG_TLS_PSK_FILE ||
			0 != (program_type & (ZBX_PROGRAM_TYPE_SERVER | ZBX_PROGRAM_TYPE_PROXY))) &&
			NULL == (ctx_psk = SSL_CTX_new(method)))
	{
		goto out_method;
	}

	/* Sometimes we need to be ready for both certificate and PSK whichever comes in. Set up a universal context */
	/* for certificate and PSK authentication to prepare for both. */
	if (NULL != ctx_cert && NULL != ctx_psk && NULL == (ctx_all = SSL_CTX_new(method)))
		goto out_method;

	/* 'TLSCaFile' parameter (in zabbix_server.conf, zabbix_proxy.conf, zabbix_agentd.conf, zabbix_agent.conf) */
	if (NULL != CONFIG_TLS_CA_FILE)
	{
		if (1 != SSL_CTX_load_verify_locations(ctx_cert, CONFIG_TLS_CA_FILE, NULL) ||
				(NULL != ctx_all && 1 != SSL_CTX_load_verify_locations(ctx_all, CONFIG_TLS_CA_FILE,
				NULL)))
		{
			zbx_snprintf_alloc(&error, &error_alloc, &error_offset, "cannot load CA certificate(s) from "
					"file \"%s\":", CONFIG_TLS_CA_FILE);
			goto out;
		}

		zabbix_log(LOG_LEVEL_DEBUG, "%s(): successfully loaded CA certificate(s)", __function_name);

		SSL_CTX_set_verify(ctx_cert, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
		SSL_CTX_set_verify_depth(ctx_cert, 1);	/* TODO is depth 1 ok ? */

		if (NULL != ctx_all)
		{
			SSL_CTX_set_verify(ctx_all, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
			SSL_CTX_set_verify_depth(ctx_all, 1);	/* TODO is depth 1 ok ? */
		}
	}

	/* 'TLSCrlFile' parameter (in zabbix_server.conf, zabbix_proxy.conf, zabbix_agentd.conf, zabbix_agent.conf). */
	/* Load CRL (certificate revocation list) file. */
	if (NULL != CONFIG_TLS_CRL_FILE)
	{
		/* TODO implement CRL functionality */
	}

	/* 'TLSCertFile' parameter (in zabbix_server.conf, zabbix_proxy.conf, zabbix_agentd.conf, zabbix_agent.conf). */
	/* Load certificate. */
	if (NULL != CONFIG_TLS_CERT_FILE)
	{
		if (1 != SSL_CTX_use_certificate_file(ctx_cert, CONFIG_TLS_CERT_FILE, SSL_FILETYPE_PEM) ||
				(NULL != ctx_all && 1 != SSL_CTX_use_certificate_file(ctx_all, CONFIG_TLS_CERT_FILE,
				SSL_FILETYPE_PEM)))
		{
			zbx_snprintf_alloc(&error, &error_alloc, &error_offset, "cannot load certificate from file "
					"\"%s\":", CONFIG_TLS_CERT_FILE);
			goto out;
		}

		zabbix_log(LOG_LEVEL_DEBUG, "%s(): successfully loaded certificate", __function_name);
	}

	/* 'TLSKeyFile' parameter (in zabbix_server.conf, zabbix_proxy.conf, zabbix_agentd.conf, zabbix_agent.conf). */
	/* Load private key. */
	if (NULL != CONFIG_TLS_KEY_FILE)
	{
		if (1 != SSL_CTX_use_PrivateKey_file(ctx_cert, CONFIG_TLS_KEY_FILE, SSL_FILETYPE_PEM) ||
				(NULL != ctx_all && 1 != SSL_CTX_use_PrivateKey_file(ctx_all, CONFIG_TLS_KEY_FILE,
				SSL_FILETYPE_PEM)))
		{
			zbx_snprintf_alloc(&error, &error_alloc, &error_offset, "cannot load private key from file "
					"\"%s\":", CONFIG_TLS_KEY_FILE);
			goto out;
		}

		zabbix_log(LOG_LEVEL_DEBUG, "%s(): successfully loaded private key", __function_name);

		if (1 != SSL_CTX_check_private_key(ctx_cert))
		{
			zbx_snprintf_alloc(&error, &error_alloc, &error_offset, "certificate and private key do not "
					"match:");
			goto out;
		}

		zabbix_log(LOG_LEVEL_DEBUG, "%s(): successfully loaded private key", __function_name);
	}

	/* 'TLSPskIdentity' and 'TLSPskFile' parameters used in zabbix_proxy.conf, zabbix_agentd.conf, */
	/* zabbix_agent.conf. Load pre-shared key and identity to be used with the pre-shared key. */

	if (NULL != CONFIG_TLS_PSK_FILE)
	{
		my_psk_identity = CONFIG_TLS_PSK_IDENTITY;
		my_psk_identity_len = strlen(my_psk_identity);

		zbx_read_psk_file();
		zabbix_log(LOG_LEVEL_DEBUG, "%s(): successfully loaded pre-shared key", __function_name);
	}

	/* set up ciphersuites */

	if (NULL != ctx_cert &&
			1 != SSL_CTX_set_cipher_list(ctx_cert, "DEFAULT:!EXPORT:!RC4:!SRP:!DES:!DSS:!SEED:!KRB5:!PSK"))
	{
		zbx_snprintf_alloc(&error, &error_alloc, &error_offset, "cannot set list of certificate ciphersuites:");
		goto out;
	}

	if (NULL != ctx_psk &&
			1 != SSL_CTX_set_cipher_list(ctx_psk, "DEFAULT:!EXPORT:!RC4:!SRP:!DES:!DSS:!SEED:!DH:!RSA:"
			"!ECDH:!KRB5"))
	{
		zbx_snprintf_alloc(&error, &error_alloc, &error_offset, "cannot set list of PSK ciphersuites:");
		goto out;
	}

	if (NULL != ctx_all &&
			1 != SSL_CTX_set_cipher_list(ctx_all, "DEFAULT:!EXPORT:!RC4:!SRP:!DES:!DSS:!SEED:!KRB5"))
	{
		zbx_snprintf_alloc(&error, &error_alloc, &error_offset, "cannot set list of all ciphersuites:");
		goto out;
	}

	/* set up PSK global variables for client callback if PSK comes only from configuration file or command line */

	if (NULL != ctx_psk && 0 != (program_type & (ZBX_PROGRAM_TYPE_AGENTD | ZBX_PROGRAM_TYPE_AGENT |
			ZBX_PROGRAM_TYPE_SENDER | ZBX_PROGRAM_TYPE_GET)))
	{
		psk_identity_for_cb = my_psk_identity;
		psk_identity_len_for_cb = my_psk_identity_len;
		psk_for_cb = my_psk;
		psk_len_for_cb = my_psk_len;
	}

	/* set up info and PSK callbacks */

	if (NULL != ctx_cert)
		SSL_CTX_set_info_callback(ctx_cert, zbx_openssl_info_callback);

	if (NULL != ctx_psk)
	{
		SSL_CTX_set_info_callback(ctx_psk, zbx_openssl_info_callback);

		if (0 != (program_type & (ZBX_PROGRAM_TYPE_SERVER | ZBX_PROGRAM_TYPE_PROXY | ZBX_PROGRAM_TYPE_AGENTD |
				ZBX_PROGRAM_TYPE_SENDER | ZBX_PROGRAM_TYPE_GET)))
		{
			SSL_CTX_set_psk_client_callback(ctx_psk, zbx_psk_client_callback);
		}

		if (0 != (program_type & (ZBX_PROGRAM_TYPE_SERVER | ZBX_PROGRAM_TYPE_PROXY | ZBX_PROGRAM_TYPE_AGENTD |
				ZBX_PROGRAM_TYPE_AGENT)))
		{
			SSL_CTX_set_psk_server_callback(ctx_psk, zbx_psk_server_callback);
		}
	}

	if (NULL != ctx_all)
	{
		SSL_CTX_set_info_callback(ctx_all, zbx_openssl_info_callback);

		if (0 != (program_type & (ZBX_PROGRAM_TYPE_SERVER | ZBX_PROGRAM_TYPE_PROXY | ZBX_PROGRAM_TYPE_AGENTD |
				ZBX_PROGRAM_TYPE_AGENT)))
		{
			SSL_CTX_set_psk_server_callback(ctx_all, zbx_psk_server_callback);
		}
	}

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);

	return;

out_method:
	zbx_snprintf_alloc(&error, &error_alloc, &error_offset, "cannot initialize TLS method:");
out:
	zbx_tls_error_msg(&error, &error_alloc, &error_offset);
	zabbix_log(LOG_LEVEL_CRIT, "%s", error);
	zbx_free(error);
	zbx_tls_free();
	exit(EXIT_FAILURE);
#endif
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_tls_free                                                     *
 *                                                                            *
 * Purpose: release TLS library resources allocated in zbx_tls_init_parent()  *
 *          and zbx_tls_init_child()                                          *
 *                                                                            *
 ******************************************************************************/
void	zbx_tls_free(void)
{
#if defined(HAVE_POLARSSL)
	if (NULL != ctr_drbg)
	{
		ctr_drbg_free(ctr_drbg);
		zbx_free(ctr_drbg);
	}

	if (NULL != entropy)
	{
		entropy_free(entropy);
		zbx_free(entropy);
	}

	if (NULL != my_psk)
	{
		zbx_guaranteed_memset(my_psk, 0, my_psk_len);
		my_psk_len = 0;
		zbx_free(my_psk);
	}

	if (NULL != my_priv_key)
	{
		pk_free(my_priv_key);
		zbx_free(my_priv_key);
	}

	if (NULL != my_cert)
	{
		x509_crt_free(my_cert);
		zbx_free(my_cert);
	}

	if (NULL != crl)
	{
		x509_crl_free(crl);
		zbx_free(crl);
	}

	if (NULL != ca_cert)
	{
		x509_crt_free(ca_cert);
		zbx_free(ca_cert);
	}

	zbx_free(ciphersuites_psk);
	zbx_free(ciphersuites_cert);
	zbx_free(ciphersuites_all);
#elif defined(HAVE_GNUTLS)
	if (NULL != my_cert_creds)
	{
		gnutls_certificate_free_credentials(my_cert_creds);
		my_cert_creds = NULL;
	}

	if (NULL != my_psk_client_creds)
	{
		gnutls_psk_free_client_credentials(my_psk_client_creds);
		my_psk_client_creds = NULL;
	}

	if (NULL != my_psk_server_creds)
	{
		gnutls_psk_free_server_credentials(my_psk_server_creds);
		my_psk_server_creds = NULL;
	}

	/* In GnuTLS versions 2.8.x (RHEL 6 uses v.2.8.5 ?) gnutls_priority_init() in case of error does not release */
	/* memory allocated for 'ciphersuites_psk' but releasing it by gnutls_priority_deinit() causes crash. In     */
	/* GnuTLS versions 3.0.x - 3.1.x (RHEL 7 uses v.3.1.18 ?) gnutls_priority_init() in case of error does       */
	/* release memory allocated for 'ciphersuites_psk' but does not set pointer to NULL. Newer GnuTLS versions   */
	/* (e.g. 3.3.8) in case of error in gnutls_priority_init() do release memory and set pointer to NULL.        */
	/* Therefore we cannot reliably release this memory using the pointer. So, we leave the memory to be cleaned */
	/* up by OS - we are in the process of exiting and the data is not secret. */

	/* do not release 'ciphersuites_cert', 'ciphersuites_psk' and 'ciphersuites_all' here using */
	/* gnutls_priority_deinit() */

	if (NULL != my_psk)
	{
		zbx_guaranteed_memset(my_psk, 0, my_psk_len);
		my_psk_len = 0;
		zbx_free(my_psk);
	}

	gnutls_global_deinit();
#elif defined(HAVE_OPENSSL)
	if (NULL != ctx_cert)
		SSL_CTX_free(ctx_cert);

	if (NULL != ctx_psk)
		SSL_CTX_free(ctx_psk);

	if (NULL != ctx_all)
		SSL_CTX_free(ctx_all);

	if (NULL != my_psk)
	{
		zbx_guaranteed_memset(my_psk, 0, my_psk_len);
		my_psk_len = 0;
		zbx_free(my_psk);
	}

	RAND_cleanup();		/* erase PRNG state */
	ERR_free_strings();
#endif
}

#if defined(HAVE_POLARSSL) || defined(HAVE_GNUTLS) || defined(HAVE_OPENSSL)
/******************************************************************************
 *                                                                            *
 * Function: zbx_tls_connect                                                  *
 *                                                                            *
 * Purpose: establish a TLS connection over an established TCP connection     *
 *                                                                            *
 * Parameters:                                                                *
 *     s           - [IN]  socket with opened connection                      *
 *     error       - [OUT] dynamically allocated memory with error message    *
 *     tls_connect - [IN]  how to connect. Allowed values:                    *
 *                         ZBX_TCP_SEC_TLS_CERT, ZBX_TCP_SEC_TLS_PSK.         *
 *     tls_arg1    - [IN]  'tls_issuer' or 'tls_psk_identity' depending on    *
 *                         value of 'tls_connect'.                            *
 *     tls_arg2    - [IN]  'tls_subject' or 'tls_psk' depending on value of   *
 *                         'tls_connect'.                                     *
 *                                                                            *
 * Return value: SUCCEED on successful TLS handshake with a valid certificate *
 *               or PSK                                                       *
 *               FAIL - an error occurred                                     *
 *                                                                            *
 ******************************************************************************/
int	zbx_tls_connect(zbx_sock_t *s, char **error, unsigned int tls_connect, char *tls_arg1, char *tls_arg2)
{
#if defined(HAVE_POLARSSL)
	const char	*__function_name = "zbx_tls_connect";
	int		ret = FAIL, res;
	const x509_crt	*peer_cert;

	if (ZBX_TCP_SEC_TLS_CERT == tls_connect)
	{
		zabbix_log(LOG_LEVEL_DEBUG, "In %s(): issuer:\"%s\" subject:\"%s\"", __function_name,
				ZBX_NULL2EMPTY_STR(tls_arg1), ZBX_NULL2EMPTY_STR(tls_arg2));

		if (NULL == ciphersuites_cert)
		{
			*error = zbx_strdup(*error, "cannot connect with TLS and certificate: no valid certificate "
					"loaded");
			goto out;
		}
	}
	else	/* pre-shared key */
	{
		zabbix_log(LOG_LEVEL_DEBUG, "In %s(): psk_identity:\"%s\"", __function_name,
				ZBX_NULL2EMPTY_STR(tls_arg1));

		if (NULL == ciphersuites_psk)
		{
			*error = zbx_strdup(*error, "cannot connect with TLS and PSK: no valid PSK loaded");
			goto out;
		}
	}

	/* set up TLS context */
	s->tls_ctx = zbx_malloc(s->tls_ctx, sizeof(ssl_context));
	memset(s->tls_ctx, 0, sizeof(ssl_context));

	if (0 != (res = ssl_init(s->tls_ctx)))
	{
		zbx_tls_error_msg(res, "ssl_init(): ", error);
		ssl_free(s->tls_ctx);
		zbx_free(s->tls_ctx);
		goto out;
	}

	ssl_set_endpoint(s->tls_ctx, SSL_IS_CLIENT);

	/* Set RNG callback where to get random numbers from */
	ssl_set_rng(s->tls_ctx, ctr_drbg_random, ctr_drbg);

	if (SUCCEED == zabbix_check_log_level(LOG_LEVEL_TRACE))
	{
		/* Set our own debug function 'polarssl_debug()' as a callback function. The 3rd parameter of */
		/* ssl_set_dbg() we set to NULL. During a calback, the 3rd parameter will be passed to our function */
		/* 'polarssl_debug()' as the 1st parameter, but it will be ignored in 'polarssl_debug()'. */
		ssl_set_dbg(s->tls_ctx, polarssl_debug, NULL);

		/* For Zabbix LOG_LEVEL_TRACE, PolarSSL debug level 3 seems the best. Recompile with 4 (apparently */
		/* the highest PolarSSL debug level) to dump also network raw bytes. */
		debug_set_threshold(3);
	}

	/* Set callback functions for receiving and sending data via socket. */
	/* Functions provided by PolarSSL work well so far, no need to invent our own. */
	ssl_set_bio(s->tls_ctx, net_recv, &s->socket, net_send, &s->socket);

	/* set protocol version to TLS 1.2 */
	ssl_set_min_version(s->tls_ctx, ZBX_TLS_MIN_MAJOR_VER, ZBX_TLS_MIN_MINOR_VER);
	ssl_set_max_version(s->tls_ctx, ZBX_TLS_MAX_MAJOR_VER, ZBX_TLS_MAX_MINOR_VER);

	if (ZBX_TCP_SEC_TLS_CERT == tls_connect)	/* use certificates */
	{
		ssl_set_authmode(s->tls_ctx, SSL_VERIFY_REQUIRED);
		ssl_set_ciphersuites(s->tls_ctx, ciphersuites_cert);

		/* set CA certificate and certificate revocation lists */
		ssl_set_ca_chain(s->tls_ctx, ca_cert, crl, NULL);	/* TODO set the 4th argument to expected peer Common Name */

		if (0 != (res = ssl_set_own_cert(s->tls_ctx, my_cert, my_priv_key)))
		{
			zbx_tls_error_msg(res, "ssl_set_own_cert(): ", error);
			ssl_free(s->tls_ctx);
			zbx_free(s->tls_ctx);
			goto out;
		}
	}
	else	/* use a pre-shared key */
	{
		ssl_set_ciphersuites(s->tls_ctx, ciphersuites_psk);

		if (NULL == tls_arg1)	/* 'tls_psk_identity' is not set */
		{
			/* set up the PSK from a configuration file (in agentd, agent (always) and a case in active */
			/* proxy when it connects to server) */

			if (0 != (res = ssl_set_psk(s->tls_ctx, (const unsigned char *)my_psk, my_psk_len,
				(const unsigned char *)my_psk_identity, my_psk_identity_len)))
			{
				zbx_tls_error_msg(res, "ssl_set_psk(): ", error);
				ssl_free(s->tls_ctx);
				zbx_free(s->tls_ctx);
				goto out;
			}
		}
		else
		{
			/* PSK comes from a database (case for a server/proxy when it connects to an agent for */
			/* passive checks, for a server when it connects to a passive proxy) */

			int	psk_len;
			char	psk_buf[HOST_TLS_PSK_LEN/2];

			if (0 >= (psk_len = zbx_psk_hex2bin((unsigned char *)tls_arg2, (unsigned char *)psk_buf,
					sizeof(psk_buf))))
			{
				*error = zbx_strdup(*error, "invalid pre-shared key");
				ssl_free(s->tls_ctx);
				zbx_free(s->tls_ctx);
				goto out;
			}

			if (0 != (res = ssl_set_psk(s->tls_ctx, (const unsigned char *)psk_buf, (size_t)psk_len,
				(const unsigned char *)tls_arg1, strlen(tls_arg1))))
			{
				zbx_tls_error_msg(res, "ssl_set_psk(): ", error);
				ssl_free(s->tls_ctx);
				zbx_free(s->tls_ctx);
				goto out;
			}
		}
	}

	while (0 != (res = ssl_handshake(s->tls_ctx)))
	{
		if (POLARSSL_ERR_NET_WANT_READ != res && POLARSSL_ERR_NET_WANT_WRITE != res)
		{
			zbx_tls_error_msg(res, "ssl_handshake(): ", error);
			ssl_free(s->tls_ctx);
			zbx_free(s->tls_ctx);
			goto out;
		}
	}

	if (ZBX_TCP_SEC_TLS_CERT == tls_connect)
	{
		if (SUCCEED == zabbix_check_log_level(LOG_LEVEL_DEBUG))
		{
			/* log peer certificate information for debugging */

			if (NULL == (peer_cert = ssl_get_peer_cert(s->tls_ctx)) || -1 == x509_crt_info(work_buf,
					sizeof(work_buf), "", peer_cert))
			{
				*error = zbx_strdup(*error, "cannot get peer certificate info");
				ssl_free(s->tls_ctx);
				zbx_free(s->tls_ctx);
				goto out;
			}

			zabbix_log(LOG_LEVEL_DEBUG, "%s(): peer certificate:\n%s", __function_name, work_buf);
		}

		/* validate peer certificate TODO: Issuer and Subject validation */

		if (0 != (res = ssl_get_verify_result(s->tls_ctx)))
		{
			int	k = 0;

			*error = zbx_strdup(*error, "invalid peer certificate: ");

			if (0 != (BADCERT_EXPIRED & res))
			{
				*error = zbx_strdcat(*error, "expired");
				k++;
			}

			if (0 != (BADCERT_REVOKED & res))
			{
				if (0 != k)
					*error = zbx_strdcat(*error, ", ");

				*error = zbx_strdcat(*error, "revoked");
				k++;
			}

			if (0 != (BADCERT_CN_MISMATCH & res))
			{
				if (0 != k)
					*error = zbx_strdcat(*error, ", ");

				*error = zbx_strdcat(*error, "Common Name mismatch");
				k++;
			}

			if (0 != (BADCERT_NOT_TRUSTED & res))
			{
				if (0 != k)
					*error = zbx_strdcat(*error, ", ");

				*error = zbx_strdcat(*error, "self-signed or not signed by a trusted CA");
			}

			ssl_free(s->tls_ctx);
			zbx_free(s->tls_ctx);
			goto out;
		}

		s->connection_type = ZBX_TCP_SEC_TLS_CERT;
	}
	else	/* pre-shared key */
	{
		s->connection_type = ZBX_TCP_SEC_TLS_PSK;

		if (SUCCEED == zabbix_check_log_level(LOG_LEVEL_DEBUG))
		{
			/* special print: s->tls_ctx->psk_identity is not '\0'-terminated */
			zabbix_log(LOG_LEVEL_DEBUG, "%s(): PSK-identity: \"%.*s\"", __function_name,
					s->tls_ctx->psk_identity_len, s->tls_ctx->psk_identity);
		}
	}

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s(): SUCCEED (established %s %s)", __function_name,
			ssl_get_version(s->tls_ctx), ssl_get_ciphersuite(s->tls_ctx));

	return SUCCEED;
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s:%s", __function_name, zbx_result_string(ret),
			ZBX_NULL2EMPTY_STR(*error));
	return ret;

#elif defined(HAVE_GNUTLS)

	const char	*__function_name = "zbx_tls_connect";
	int		ret = FAIL, res;

	if (ZBX_TCP_SEC_TLS_CERT == tls_connect)
	{
		zabbix_log(LOG_LEVEL_DEBUG, "In %s(): issuer:\"%s\" subject:\"%s\"", __function_name,
				ZBX_NULL2EMPTY_STR(tls_arg1), ZBX_NULL2EMPTY_STR(tls_arg2));
	}
	else	/* pre-shared key */
	{
		zabbix_log(LOG_LEVEL_DEBUG, "In %s(): psk_identity:\"%s\"", __function_name,
				ZBX_NULL2EMPTY_STR(tls_arg1));
	}

	/* set up TLS context */

	s->tls_psk_client_creds = NULL;
	s->tls_psk_server_creds = NULL;

	if (GNUTLS_E_SUCCESS != (res = gnutls_init(&s->tls_ctx, GNUTLS_CLIENT))) /* TODO use GNUTLS_NO_EXTENSIONS ? */
	{
		*error = zbx_dsprintf(*error, "gnutls_init() failed: %d %s", res, gnutls_strerror(res));
		goto out;
	}

	if (ZBX_TCP_SEC_TLS_CERT == tls_connect)
	{
		if (NULL == ciphersuites_cert)
		{
			*error = zbx_strdup(*error, "cannot connect with TLS and certificate: no valid certificate "
					"loaded");
			goto out;
		}

		if (GNUTLS_E_SUCCESS != (res = gnutls_priority_set(s->tls_ctx, ciphersuites_cert)))
		{
			*error = zbx_dsprintf(*error, "gnutls_priority_set() for 'ciphersuites_cert' failed: %d %s",
					res, gnutls_strerror(res));
			goto out;
		}

		if (GNUTLS_E_SUCCESS != (res = gnutls_credentials_set(s->tls_ctx, GNUTLS_CRD_CERTIFICATE,
				my_cert_creds)))
		{
			*error = zbx_dsprintf(*error, "gnutls_credentials_set() for certificate failed: %d %s", res,
					gnutls_strerror(res));
			goto out;
		}
	}
	else	/* use a pre-shared key */
	{
		if (NULL == ciphersuites_psk)
		{
			*error = zbx_strdup(*error, "cannot connect with TLS and PSK: no valid PSK loaded");
			goto out;
		}

		if (GNUTLS_E_SUCCESS != (res = gnutls_priority_set(s->tls_ctx, ciphersuites_psk)))
		{
			*error = zbx_dsprintf(*error, "gnutls_priority_set() for 'ciphersuites_psk' failed: %d %s", res,
					gnutls_strerror(res));
			goto out;
		}

		if (NULL == tls_arg1)	/* 'tls_psk_identity' is not set */
		{
			/* set up the PSK from a configuration file (in agentd, agent (always) and a case in active */
			/* proxy when it connects to server) */

			if (GNUTLS_E_SUCCESS != (res = gnutls_credentials_set(s->tls_ctx, GNUTLS_CRD_PSK,
					my_psk_client_creds)))
			{
				*error = zbx_dsprintf(*error, "gnutls_credentials_set() for psk failed: %d %s", res,
						gnutls_strerror(res));
				goto out;
			}
		}
		else
		{
			/* PSK comes from a database (case for a server/proxy when it connects to an agent for */
			/* passive checks, for a server when it connects to a passive proxy) */

			gnutls_datum_t	key;
			int		psk_len;
			unsigned char	psk_buf[HOST_TLS_PSK_LEN/2];

			if (0 >= (psk_len = zbx_psk_hex2bin((unsigned char *)tls_arg2, psk_buf,
					sizeof(psk_buf))))
			{
				*error = zbx_strdup(*error, "invalid pre-shared key");
				goto out;
			}

			if (GNUTLS_E_SUCCESS != (res = gnutls_psk_allocate_client_credentials(
					&s->tls_psk_client_creds)))
			{
				*error = zbx_dsprintf(*error, "gnutls_psk_allocate_client_credentials() failed: %d %s",
						res, gnutls_strerror(res));
				goto out;
			}

			key.data = psk_buf;
			key.size = (unsigned int)psk_len;

			/* TODO docs say 'my_psk_identity' should be prepared with "SASLprep" profile of "stringprep" */
			if (GNUTLS_E_SUCCESS != (res = gnutls_psk_set_client_credentials(s->tls_psk_client_creds,
					tls_arg1, &key, GNUTLS_PSK_KEY_RAW)))
			{
				*error = zbx_dsprintf(*error, "gnutls_psk_set_client_credentials() failed: %d %s", res,
						gnutls_strerror(res));
				goto out;
			}

			if (GNUTLS_E_SUCCESS != (res = gnutls_credentials_set(s->tls_ctx, GNUTLS_CRD_PSK,
					s->tls_psk_client_creds)))
			{
				*error = zbx_dsprintf(*error, "gnutls_credentials_set() for psk failed: %d %s", res,
						gnutls_strerror(res));
				goto out;
			}
		}
	}

	if (SUCCEED == zabbix_check_log_level(LOG_LEVEL_TRACE))
	{
		/* set our own debug function 'zbx_gnutls_debug()' as a callback function */
		gnutls_global_set_log_function(zbx_gnutls_debug);

		/* for Zabbix LOG_LEVEL_TRACE, GnuTLS debug level 4 seems the best */
		/* (the highest GnuTLS debug level is 9) */
		gnutls_global_set_log_level(4);
	}
	else
		gnutls_global_set_log_level(0);		/* restore default log level */

#if GNUTLS_VERSION_NUMBER >= 0x030000
	/* set our own function 'zbx_gnutls_audit()' as a callback function to log issues into Zabbix log */
	gnutls_global_set_audit_log_function(zbx_gnutls_audit);
#endif

	/* on MS Windows gl_fd_to_handle() may be necessary to convert s->socket */
	gnutls_transport_set_ptr(s->tls_ctx, (gnutls_transport_ptr_t)s->socket);

	/* TLS handshake */

	while (GNUTLS_E_SUCCESS != (res = gnutls_handshake(s->tls_ctx)))
	{
		if (GNUTLS_E_INTERRUPTED == res || GNUTLS_E_AGAIN == res)
		{
			continue;
		}
		else if (GNUTLS_E_WARNING_ALERT_RECEIVED == res || GNUTLS_E_FATAL_ALERT_RECEIVED == res)
		{
			const char	*msg;
			int		alert;

			/* server sent an alert to us */
			alert = gnutls_alert_get(s->tls_ctx);

			if (NULL == (msg = gnutls_alert_get_name(alert)))
				msg = "unknown";

			if (GNUTLS_E_WARNING_ALERT_RECEIVED == res)
			{
				zabbix_log(LOG_LEVEL_WARNING, "%s(): gnutls_handshake() received a warning alert: %d "
						"%s", __function_name, alert, msg);
				continue;
			}
			else	/* GNUTLS_E_FATAL_ALERT_RECEIVED */
			{
				*error = zbx_dsprintf(*error, "%s(): gnutls_handshake() failed with fatal alert: %d %s",
						__function_name, alert, msg);
				goto out;
			}
		}
		else
		{
			zabbix_log(LOG_LEVEL_WARNING, "%s(): gnutls_handshake() returned: %d %s",
					__function_name, res, gnutls_strerror(res));

			if (0 != gnutls_error_is_fatal(res))
			{
				*error = zbx_dsprintf(*error, "%s(): gnutls_handshake() failed: %d %s",
						__function_name, res, gnutls_strerror(res));
				goto out;
			}
		}
	}

	if (ZBX_TCP_SEC_TLS_CERT == tls_connect)
	{
		/* TODO log peer certificate information for debugging */

		/* validate peer certificate TODO: Issuer and Subject validation */

		s->connection_type = ZBX_TCP_SEC_TLS_CERT;
	}
	else	/* pre-shared key */
		s->connection_type = ZBX_TCP_SEC_TLS_PSK;

	if (SUCCEED == zabbix_check_log_level(LOG_LEVEL_DEBUG))
	{
		zabbix_log(LOG_LEVEL_DEBUG, "End of %s(): SUCCEED (established %s %s-%s-%s-%zu-%zu)", __function_name,
				gnutls_protocol_get_name(gnutls_protocol_get_version(s->tls_ctx)),
				gnutls_kx_get_name(gnutls_kx_get(s->tls_ctx)),
				gnutls_cipher_get_name(gnutls_cipher_get(s->tls_ctx)),
				gnutls_mac_get_name(gnutls_mac_get(s->tls_ctx)),
				gnutls_mac_get_key_size(gnutls_mac_get(s->tls_ctx)),
				gnutls_cipher_get_key_size(gnutls_cipher_get(s->tls_ctx)));
	}

	return SUCCEED;

out:	/* an error occured */
	if (NULL != s->tls_ctx)
	{
		gnutls_credentials_clear(s->tls_ctx);
		gnutls_deinit(s->tls_ctx);
		s->tls_ctx = NULL;
	}

	if (NULL != s->tls_psk_client_creds)
	{
		gnutls_psk_free_client_credentials(s->tls_psk_client_creds);
		s->tls_psk_client_creds = NULL;
	}

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s:%s", __function_name, zbx_result_string(ret),
			ZBX_NULL2EMPTY_STR(*error));
	return ret;

#elif defined(HAVE_OPENSSL)
	const char	*__function_name = "zbx_tls_connect";
	int		ret = FAIL, res;
	size_t		error_alloc = 0, error_offset = 0;
	char		psk_buf[HOST_TLS_PSK_LEN/2];

	if (ZBX_TCP_SEC_TLS_CERT == tls_connect)
	{
		zabbix_log(LOG_LEVEL_DEBUG, "In %s(): issuer:\"%s\" subject:\"%s\"", __function_name,
				ZBX_NULL2EMPTY_STR(tls_arg1), ZBX_NULL2EMPTY_STR(tls_arg2));

		if (NULL == ctx_cert)
		{
			*error = zbx_strdup(*error, "cannot connect with TLS and certificate: no valid certificate "
					"loaded");
			goto out;
		}

		if (NULL == (s->tls_ctx = SSL_new(ctx_cert)))
		{
			zbx_snprintf_alloc(error, &error_alloc, &error_offset, "cannot create connection context:");
			zbx_tls_error_msg(error, &error_alloc, &error_offset);
			goto out;
		}
	}
	else	/* connect with pre-shared key */
	{
		zabbix_log(LOG_LEVEL_DEBUG, "In %s(): psk_identity:\"%s\"", __function_name,
				ZBX_NULL2EMPTY_STR(tls_arg1));

		if (NULL == ctx_psk)
		{
			*error = zbx_strdup(*error, "cannot connect with TLS and PSK: no valid PSK loaded");
			goto out;
		}

		if (NULL == (s->tls_ctx = SSL_new(ctx_psk)))
		{
			zbx_snprintf_alloc(error, &error_alloc, &error_offset, "cannot create connection context:");
			zbx_tls_error_msg(error, &error_alloc, &error_offset);
			goto out;
		}

		if (NULL == tls_arg1)	/* 'tls_psk_identity' is not set from DB */
		{
			/* Set up PSK global variables from a configuration file (always in agentd, agent and a */
			/* case when active proxy connects to server). Here we set it only in case of active proxy */
			/* because for other programs it already has been set in zbx_tls_init_child(). */

			if (0 != (program_type & ZBX_PROGRAM_TYPE_PROXY_ACTIVE))
			{
				psk_identity_for_cb = my_psk_identity;
				psk_identity_len_for_cb = my_psk_identity_len;
				psk_for_cb = my_psk;
				psk_len_for_cb = my_psk_len;
			}
		}
		else
		{
			/* PSK comes from a database (case for a server/proxy when it connects to an agent for */
			/* passive checks, for a server when it connects to a passive proxy) */

			int	psk_len;

			if (0 >= (psk_len = zbx_psk_hex2bin((unsigned char *)tls_arg2, (unsigned char *)psk_buf,
					sizeof(psk_buf))))
			{
				*error = zbx_strdup(*error, "invalid pre-shared key");
				goto out;
			}

			/* some data reside in stack but it will be available at the time when a PSK client callback */
			/* function copies the data into buffers provided by OpenSSL within the callback */
			psk_identity_for_cb = tls_arg1;			/* string is on stack */
			psk_identity_len_for_cb = strlen(tls_arg1);
			psk_for_cb = psk_buf;				/* buffer is on stack */
			psk_len_for_cb = (size_t)psk_len;
		}
	}

	/* set our connected TCP socket to TLS context */
	if (1 != SSL_set_fd(s->tls_ctx, s->socket))
	{
		*error = zbx_strdup(*error, "cannot set socket for TLS context");
		goto out;
	}

	zbx_log_ciphersuites(__function_name, s->tls_ctx);

	/* TLS handshake */

	info_buf[0] = '\0';	/* empty buffer for zbx_openssl_info_callback() messages */

	if (1 != (res = SSL_connect(s->tls_ctx)))
	{
		int	error_code;

		error_code = SSL_get_error(s->tls_ctx, res);

		switch (error_code)
		{
			case SSL_ERROR_NONE:		/* handshake successful */
				break;
			case SSL_ERROR_ZERO_RETURN:	/* TLS connection has been closed */
				*error = zbx_strdup(*error, "TLS connection has been closed during handshake");
				goto out;
				break;
			case SSL_ERROR_SYSCALL:
				zbx_snprintf_alloc(error, &error_alloc, &error_offset, "TLS handshake returned "
						"SSL_ERROR_SYSCALL:");
				zbx_tls_error_msg(error, &error_alloc, &error_offset);
				zbx_snprintf_alloc(error, &error_alloc, &error_offset, ": %s", info_buf);
				goto out;
				break;
			case SSL_ERROR_SSL:
				zbx_snprintf_alloc(error, &error_alloc, &error_offset, "TLS handshake returned "
						"SSL_ERROR_SSL:");
				zbx_tls_error_msg(error, &error_alloc, &error_offset);
				zbx_snprintf_alloc(error, &error_alloc, &error_offset, ": %s", info_buf);
				goto out;
				break;
			default:
				zbx_snprintf_alloc(error, &error_alloc, &error_offset, "TLS handshake returned error "
						"code %d", error_code);
				zbx_tls_error_msg(error, &error_alloc, &error_offset);
				zbx_snprintf_alloc(error, &error_alloc, &error_offset, ": %s", info_buf);
				goto out;
		}
	}

	if (ZBX_TCP_SEC_TLS_CERT == tls_connect)
	{
		/* TODO log peer certificate information for debugging */

		/* validate peer certificate TODO: Issuer and Subject validation */

		s->connection_type = ZBX_TCP_SEC_TLS_CERT;
	}
	else	/* pre-shared key */
		s->connection_type = ZBX_TCP_SEC_TLS_PSK;

	if (SUCCEED == zabbix_check_log_level(LOG_LEVEL_DEBUG))
	{
		zabbix_log(LOG_LEVEL_DEBUG, "End of %s(): SUCCEED (established %s %s)", __function_name,
				SSL_get_version(s->tls_ctx), SSL_get_cipher(s->tls_ctx));
	}

	return SUCCEED;
out:
	if (NULL != s->tls_ctx)
	{
		SSL_free(s->tls_ctx);
		s->tls_ctx = NULL;
	}

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s:%s", __function_name, zbx_result_string(ret),
			ZBX_NULL2EMPTY_STR(*error));
	return ret;
#endif
}
#endif

#if defined(HAVE_POLARSSL) || defined(HAVE_GNUTLS) || defined(HAVE_OPENSSL)
/******************************************************************************
 *                                                                            *
 * Function: zbx_tls_accept                                                   *
 *                                                                            *
 * Purpose: establish a TLS connection over an accepted TCP connection        *
 *                                                                            *
 * Parameters:                                                                *
 *     s          - [IN]  socket with opened connection                       *
 *     error      - [OUT] dynamically allocated memory with error message     *
 *     tls_accept - [IN]  what connection to accept. Can be be either         *
 *                        ZBX_TCP_SEC_TLS_CERT or ZBX_TCP_SEC_TLS_PSK, or     *
 *                        a bitwise 'OR' of both.                             *
 *                                                                            *
 * Return value: SUCCEED on successful TLS handshake with a valid certificate *
 *               or PSK                                                       *
 *               FAIL - an error occurred                                     *
 *                                                                            *
 ******************************************************************************/
int	zbx_tls_accept(zbx_sock_t *s, char **error, unsigned int tls_accept)
{
#if defined(HAVE_POLARSSL)
	const char		*__function_name = "zbx_tls_accept";
	int			ret = FAIL, res;
	const x509_crt		*peer_cert;
	const ssl_ciphersuite_t	*info;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	if (0 != (tls_accept & ZBX_TCP_SEC_TLS_PSK) && NULL == ciphersuites_psk)
	{
		*error = zbx_strdup(*error, "cannot accept TLS connection with PSK: no valid PSK loaded");
		goto out;
	}

	/* set up TLS context */
	s->tls_ctx = zbx_malloc(s->tls_ctx, sizeof(ssl_context));
	memset(s->tls_ctx, 0, sizeof(ssl_context));

	if (0 != (res = ssl_init(s->tls_ctx)))
	{
		zbx_tls_error_msg(res, "ssl_init(): ", error);
		ssl_free(s->tls_ctx);
		zbx_free(s->tls_ctx);
		goto out;
	}

	ssl_set_endpoint(s->tls_ctx, SSL_IS_SERVER);

	/* Set RNG callback where to get random numbers from */
	ssl_set_rng(s->tls_ctx, ctr_drbg_random, ctr_drbg);

	if (SUCCEED == zabbix_check_log_level(LOG_LEVEL_TRACE))
	{
		/* Set our own debug function 'polarssl_debug()' as a callback function. The 3rd parameter of */
		/* ssl_set_dbg() we set to NULL. During a calback, the 3rd parameter will be passed to our function */
		/* 'polarssl_debug()' as the 1st parameter, but it will be ignored in 'polarssl_debug()'. */
		ssl_set_dbg(s->tls_ctx, polarssl_debug, NULL);

		/* For Zabbix LOG_LEVEL_TRACE, PolarSSL debug level 3 seems the best. Recompile with 4 (apparently */
		/* the highest PolarSSL debug level) to dump also network raw bytes. */
		debug_set_threshold(3);
	}

	/* Set callback functions for receiving and sending data via socket. */
	/* Functions provided by PolarSSL work well so far, no need to invent our own. */
	ssl_set_bio(s->tls_ctx, net_recv, &s->socket, net_send, &s->socket);

	/* set protocol version to TLS 1.2 */
	ssl_set_min_version(s->tls_ctx, ZBX_TLS_MIN_MAJOR_VER, ZBX_TLS_MIN_MINOR_VER);
	ssl_set_max_version(s->tls_ctx, ZBX_TLS_MAX_MAJOR_VER, ZBX_TLS_MAX_MINOR_VER);

	/* prepare to accept with certificate */

	if (0 != (tls_accept & ZBX_TCP_SEC_TLS_CERT))
	{
		ssl_set_authmode(s->tls_ctx, SSL_VERIFY_REQUIRED);

		/* set CA certificate and certificate revocation lists */
		if (NULL != ca_cert)
			ssl_set_ca_chain(s->tls_ctx, ca_cert, crl, NULL);	/* TODO set the 4th argument to expected peer Common Name ? */

		if (NULL != my_cert && 0 != (res = ssl_set_own_cert(s->tls_ctx, my_cert, my_priv_key)))
		{
			zbx_tls_error_msg(res, "ssl_set_own_cert(): ", error);
			ssl_free(s->tls_ctx);
			zbx_free(s->tls_ctx);
			goto out;
		}
	}

	/* prepare te accept with pre-shared key */

	if (0 != (tls_accept & ZBX_TCP_SEC_TLS_PSK))
	{
		/* for agentd and agent the only possibility is a PSK from configuration file */
		if (0 != (program_type & (ZBX_PROGRAM_TYPE_AGENTD | ZBX_PROGRAM_TYPE_AGENT)) &&
				0 != (res = ssl_set_psk(s->tls_ctx, (const unsigned char *)my_psk, my_psk_len,
				(const unsigned char *)my_psk_identity, my_psk_identity_len)))
		{
			zbx_tls_error_msg(res, "ssl_set_psk(): ", error);
			ssl_free(s->tls_ctx);
			zbx_free(s->tls_ctx);
			goto out;
		}
		else if (0 != (program_type & (ZBX_PROGRAM_TYPE_PROXY | ZBX_PROGRAM_TYPE_SERVER)))
		{
			/* For server or proxy a PSK can come from configuration file or database. */
			/* Set up a callback function for finding the requested PSK. */
			ssl_set_psk_cb(s->tls_ctx, zbx_psk_callback, NULL);
		}
	}

	/* set up ciphersuites */

	if ((ZBX_TCP_SEC_TLS_CERT | ZBX_TCP_SEC_TLS_PSK) == (tls_accept & (ZBX_TCP_SEC_TLS_CERT | ZBX_TCP_SEC_TLS_PSK)))
	{
		/* common case in trapper - be ready for all types of incoming connections */
		if (NULL != my_cert)
		{
			/* it can also be a case in agentd listener - when both certificate and PSK is allowed, e.g. */
			/* for switching of TLS connections from PSK to using a certificate */
			ssl_set_ciphersuites(s->tls_ctx, ciphersuites_all);
		}
		else
		{
			/* assume PSK, although it is not yet known will there be the right PSK available */
			ssl_set_ciphersuites(s->tls_ctx, ciphersuites_psk);
		}
	}
	else if (0 != (tls_accept & ZBX_TCP_SEC_TLS_CERT) && NULL != my_cert)
		ssl_set_ciphersuites(s->tls_ctx, ciphersuites_cert);
	else if (0 != (tls_accept & ZBX_TCP_SEC_TLS_PSK))
		ssl_set_ciphersuites(s->tls_ctx, ciphersuites_psk);

	/* TLS handshake */

	while (0 != (res = ssl_handshake(s->tls_ctx)))
	{
		if (POLARSSL_ERR_NET_WANT_READ != res && POLARSSL_ERR_NET_WANT_WRITE != res)
		{
			zbx_tls_error_msg(res, "ssl_handshake(): ", error);
			ssl_free(s->tls_ctx);
			zbx_free(s->tls_ctx);
			goto out;
		}
	}

	/* Is this TLS conection using certificate or PSK ? */

	info = ssl_ciphersuite_from_id(s->tls_ctx->session->ciphersuite);

	if (POLARSSL_KEY_EXCHANGE_PSK == info->key_exchange ||
			POLARSSL_KEY_EXCHANGE_DHE_PSK == info->key_exchange ||
			POLARSSL_KEY_EXCHANGE_ECDHE_PSK == info->key_exchange ||
			POLARSSL_KEY_EXCHANGE_RSA_PSK == info->key_exchange)
	{
		s->connection_type = ZBX_TCP_SEC_TLS_PSK;

		if (SUCCEED == zabbix_check_log_level(LOG_LEVEL_DEBUG))
		{
			/* special print: s->tls_ctx->psk_identity is not '\0'-terminated */
			zabbix_log(LOG_LEVEL_DEBUG, "%s(): PSK-identity: \"%.*s\"", __function_name,
					s->tls_ctx->psk_identity_len, s->tls_ctx->psk_identity);
		}
	}
	else
	{
		s->connection_type = ZBX_TCP_SEC_TLS_CERT;

		if (SUCCEED == zabbix_check_log_level(LOG_LEVEL_DEBUG))
		{
			/* log peer certificate information for debugging */

			if (NULL == (peer_cert = ssl_get_peer_cert(s->tls_ctx)) || -1 == x509_crt_info(work_buf,
					sizeof(work_buf), "", peer_cert))
			{
				*error = zbx_strdup(*error, "cannot get peer certificate info");
				ssl_free(s->tls_ctx);
				zbx_free(s->tls_ctx);
				goto out;
			}

			zabbix_log(LOG_LEVEL_DEBUG, "%s(): peer certificate:\n%s", __function_name, work_buf);
		}

		/* validate peer certificate TODO: Issuer and Subject validation */

		if (0 != (res = ssl_get_verify_result(s->tls_ctx)))
		{
			int	k = 0;

			*error = zbx_strdup(*error, "invalid peer certificate: ");

			if (0 != (BADCERT_EXPIRED & res))
			{
				*error = zbx_strdcat(*error, "expired");
				k++;
			}

			if (0 != (BADCERT_REVOKED & res))
			{
				if (0 != k)
					*error = zbx_strdcat(*error, ", ");

				*error = zbx_strdcat(*error, "revoked");
				k++;
			}

			if (0 != (BADCERT_CN_MISMATCH & res))
			{
				if (0 != k)
					*error = zbx_strdcat(*error, ", ");

				*error = zbx_strdcat(*error, "Common Name mismatch");
				k++;
			}

			if (0 != (BADCERT_NOT_TRUSTED & res))
			{
				if (0 != k)
					*error = zbx_strdcat(*error, ", ");

				*error = zbx_strdcat(*error, "self-signed or not signed by a trusted CA");
			}

			ssl_free(s->tls_ctx);
			zbx_free(s->tls_ctx);
			goto out;
		}
	}

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s(): SUCCEED (established %s %s)", __function_name,
			ssl_get_version(s->tls_ctx), ssl_get_ciphersuite(s->tls_ctx));

	return SUCCEED;
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s:%s", __function_name, zbx_result_string(ret),
			ZBX_NULL2EMPTY_STR(*error));
	return ret;

#elif defined(HAVE_GNUTLS)

	const char			*__function_name = "zbx_tls_accept";
	int				ret = FAIL, res;
	gnutls_credentials_type_t	creds;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	/* set up TLS context */

	s->tls_psk_client_creds = NULL;
	s->tls_psk_server_creds = NULL;

	if (GNUTLS_E_SUCCESS != (res = gnutls_init(&s->tls_ctx, GNUTLS_SERVER))) /* TODO use GNUTLS_NO_EXTENSIONS ? */
	{
		*error = zbx_dsprintf(*error, "gnutls_init() failed: %d %s", res, gnutls_strerror(res));
		goto out;
	}

	/* prepare to accept with certificate */

	if (0 != (tls_accept & ZBX_TCP_SEC_TLS_CERT))
	{
		if (NULL != my_cert_creds && GNUTLS_E_SUCCESS != (res = gnutls_credentials_set(s->tls_ctx,
				GNUTLS_CRD_CERTIFICATE, my_cert_creds)))
		{
			*error = zbx_dsprintf(*error, "gnutls_credentials_set() for certificate failed: %d %s", res,
					gnutls_strerror(res));
			goto out;
		}

		/* client certificate is mandatory unless pre-shared key is used */
		gnutls_certificate_server_set_request(s->tls_ctx, GNUTLS_CERT_REQUIRE);
	}

	/* prepare te accept with pre-shared key */

	if (0 != (tls_accept & ZBX_TCP_SEC_TLS_PSK))
	{
		/* for agentd and agent the only possibility is a PSK from configuration file */
		if (0 != (program_type & (ZBX_PROGRAM_TYPE_AGENTD | ZBX_PROGRAM_TYPE_AGENT)) &&
				GNUTLS_E_SUCCESS != (res = gnutls_credentials_set(s->tls_ctx, GNUTLS_CRD_PSK,
				my_psk_server_creds)))
		{
			*error = zbx_dsprintf(*error, "gnutls_credentials_set() for my_psk_server_creds failed: %d %s",
					res, gnutls_strerror(res));
			goto out;
		}
		else if (0 != (program_type & (ZBX_PROGRAM_TYPE_PROXY | ZBX_PROGRAM_TYPE_SERVER)))
		{
			/* For server or proxy a PSK can come from configuration file or database. */
			/* Set up a callback function for finding the requested PSK. */
			if (0 != (res = gnutls_psk_allocate_server_credentials(&s->tls_psk_server_creds)))
			{
				*error = zbx_dsprintf(*error, "gnutls_psk_allocate_server_credentials() for "
						"tls_psk_server_creds failed: %d %s", res, gnutls_strerror(res));
				goto out;
			}

			gnutls_psk_set_server_credentials_function(s->tls_psk_server_creds, zbx_psk_callback);

			if (GNUTLS_E_SUCCESS != (res = gnutls_credentials_set(s->tls_ctx, GNUTLS_CRD_PSK,
					s->tls_psk_server_creds)))
			{
				*error = zbx_dsprintf(*error, "gnutls_credentials_set() for tls_psk_server_creds failed"
						": %d %s", res, gnutls_strerror(res));
				goto out;
			}
		}
	}

	/* set up ciphersuites */

	if ((ZBX_TCP_SEC_TLS_CERT | ZBX_TCP_SEC_TLS_PSK) == (tls_accept & (ZBX_TCP_SEC_TLS_CERT | ZBX_TCP_SEC_TLS_PSK)))
	{
		/* common case in trapper - be ready for all types of incoming connections */
		if (NULL != my_cert_creds)
		{
			/* it can also be a case in agentd listener - when both certificate and PSK is allowed, e.g. */
			/* for switching of TLS connections from PSK to using a certificate */
			if (GNUTLS_E_SUCCESS != (res = gnutls_priority_set(s->tls_ctx, ciphersuites_all)))
			{
				*error = zbx_dsprintf(*error, "gnutls_priority_set() for 'ciphersuites_all' failed: %d "
						"%s", res, gnutls_strerror(res));
				goto out;
			}
		}
		else
		{
			/* assume PSK, although it is not yet known will there be the right PSK available */
			if (GNUTLS_E_SUCCESS != (res = gnutls_priority_set(s->tls_ctx, ciphersuites_psk)))
			{
				*error = zbx_dsprintf(*error, "gnutls_priority_set() for 'ciphersuites_psk' failed: %d "
						"%s", res, gnutls_strerror(res));
				goto out;
			}
		}
	}
	else if (0 != (tls_accept & ZBX_TCP_SEC_TLS_CERT) && NULL != my_cert_creds)
	{
		if (GNUTLS_E_SUCCESS != (res = gnutls_priority_set(s->tls_ctx, ciphersuites_cert)))
		{
			*error = zbx_dsprintf(*error, "gnutls_priority_set() for 'ciphersuites_cert' failed: %d %s",
					res, gnutls_strerror(res));
			goto out;
		}
	}
	else if (0 != (tls_accept & ZBX_TCP_SEC_TLS_PSK))
	{
		if (GNUTLS_E_SUCCESS != (res = gnutls_priority_set(s->tls_ctx, ciphersuites_psk)))
		{
			*error = zbx_dsprintf(*error, "gnutls_priority_set() for 'ciphersuites_psk' failed: %d "
					"%s", res, gnutls_strerror(res));
			goto out;
		}
	}

	if (SUCCEED == zabbix_check_log_level(LOG_LEVEL_TRACE))
	{
		/* set our own debug function 'zbx_gnutls_debug()' as a callback function */
		gnutls_global_set_log_function(zbx_gnutls_debug);

		/* for Zabbix LOG_LEVEL_TRACE, GnuTLS debug level 4 seems the best */
		/* (the highest GnuTLS debug level is 9) */
		gnutls_global_set_log_level(4);
	}
	else
		gnutls_global_set_log_level(0);		/* restore default log level */

#if GNUTLS_VERSION_NUMBER >= 0x030000
	/* set our own function 'zbx_gnutls_audit()' as a callback function to log issues into Zabbix log */
	gnutls_global_set_audit_log_function(zbx_gnutls_audit);
#endif

	/* on MS Windows gl_fd_to_handle() may be necessary to convert s->socket */
	gnutls_transport_set_ptr(s->tls_ctx, (gnutls_transport_ptr_t)s->socket);

	/* TLS handshake */

	while (GNUTLS_E_SUCCESS != (res = gnutls_handshake(s->tls_ctx)))
	{
		if (GNUTLS_E_INTERRUPTED == res || GNUTLS_E_AGAIN == res)
		{
			continue;
		}
		else if (GNUTLS_E_WARNING_ALERT_RECEIVED == res || GNUTLS_E_FATAL_ALERT_RECEIVED == res)
		{
			const char	*msg;
			int		alert;

			/* client sent an alert to us */
			alert = gnutls_alert_get(s->tls_ctx);

			if (NULL == (msg = gnutls_alert_get_name(alert)))
				msg = "unknown";

			if (GNUTLS_E_WARNING_ALERT_RECEIVED == res)
			{
				zabbix_log(LOG_LEVEL_WARNING, "%s(): gnutls_handshake() received a warning alert: %d "
						"%s", __function_name, alert, msg);
				continue;
			}
			else	/* GNUTLS_E_FATAL_ALERT_RECEIVED */
			{
				*error = zbx_dsprintf(*error, "%s(): gnutls_handshake() failed with fatal alert: %d %s",
						__function_name, alert, msg);
				goto out;
			}
		}
		else	/* TODO should we process GNUTLS_E_GOT_APPLICATION_DATA ? */
		{
			zabbix_log(LOG_LEVEL_WARNING, "%s(): gnutls_handshake() returned: %d %s",
					__function_name, res, gnutls_strerror(res));

			if (0 != gnutls_error_is_fatal(res))
			{
				*error = zbx_dsprintf(*error, "%s(): gnutls_handshake() failed: %d %s",
					__function_name, res, gnutls_strerror(res));
				goto out;
			}
		}
	}

	/* Is this TLS conection using certificate or PSK ? */

	if (GNUTLS_CRD_CERTIFICATE == (creds = gnutls_auth_get_type(s->tls_ctx)))
	{
		s->connection_type = ZBX_TCP_SEC_TLS_CERT;

		/* TODO log some certificate debug info here */

		/* validate peer certificate TODO: Issuer and Subject validation */
	}
	else if (GNUTLS_CRD_PSK == creds)
	{
		s->connection_type = ZBX_TCP_SEC_TLS_PSK;

		if (SUCCEED == zabbix_check_log_level(LOG_LEVEL_DEBUG))
		{
			const char	*psk_identity;

			if (NULL != (psk_identity = gnutls_psk_server_get_username(s->tls_ctx)))
			{
				zabbix_log(LOG_LEVEL_DEBUG, "%s(): PSK-identity: \"%s\"", __function_name,
						psk_identity);
			}
		}
	}
	else
	{
		THIS_SHOULD_NEVER_HAPPEN;
		zbx_tls_close(s);
		return FAIL;
	}

	if (SUCCEED == zabbix_check_log_level(LOG_LEVEL_DEBUG))
	{
		zabbix_log(LOG_LEVEL_DEBUG, "End of %s(): SUCCEED (established %s %s-%s-%s-%zu-%zu)", __function_name,
				gnutls_protocol_get_name(gnutls_protocol_get_version(s->tls_ctx)),
				gnutls_kx_get_name(gnutls_kx_get(s->tls_ctx)),
				gnutls_cipher_get_name(gnutls_cipher_get(s->tls_ctx)),
				gnutls_mac_get_name(gnutls_mac_get(s->tls_ctx)),
				gnutls_mac_get_key_size(gnutls_mac_get(s->tls_ctx)),
				gnutls_cipher_get_key_size(gnutls_cipher_get(s->tls_ctx)));
	}

	return SUCCEED;

out:	/* an error occured */
	if (NULL != s->tls_ctx)
	{
		gnutls_credentials_clear(s->tls_ctx);
		gnutls_deinit(s->tls_ctx);
		s->tls_ctx = NULL;
	}

	if (NULL != s->tls_psk_server_creds)
	{
		gnutls_psk_free_server_credentials(s->tls_psk_server_creds);
		s->tls_psk_server_creds = NULL;
	}

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s:%s", __function_name, zbx_result_string(ret),
			ZBX_NULL2EMPTY_STR(*error));
	return ret;

#elif defined(HAVE_OPENSSL)
	const char	*__function_name = "zbx_tls_accept", *cipher_name;
	int		ret = FAIL, res;
	size_t		error_alloc = 0, error_offset = 0;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	if ((ZBX_TCP_SEC_TLS_CERT | ZBX_TCP_SEC_TLS_PSK) == (tls_accept & (ZBX_TCP_SEC_TLS_CERT | ZBX_TCP_SEC_TLS_PSK)))
	{
		/* common case in trapper - be ready for all types of incoming connections but possible also in */
		/* agentd/agent listener */

		if (NULL != ctx_all)
		{
			if (NULL == (s->tls_ctx = SSL_new(ctx_all)))
			{
				zbx_snprintf_alloc(error, &error_alloc, &error_offset, "cannot create context to accept"
						" connection:");
				zbx_tls_error_msg(error, &error_alloc, &error_offset);
				goto out;
			}
		}
		else if (0 != (program_type & (ZBX_PROGRAM_TYPE_AGENTD | ZBX_PROGRAM_TYPE_AGENT)))
		{
			zbx_snprintf_alloc(error, &error_alloc, &error_offset, "not ready for both certificate and "
					"PSK-based incoming connection:");
			if (NULL == ctx_cert)
				zbx_snprintf_alloc(error, &error_alloc, &error_offset, " certificate not loaded");

			if (NULL == ctx_psk)
				zbx_snprintf_alloc(error, &error_alloc, &error_offset, " PSK not loaded");

			goto out;
		}

		/* Server or proxy with no certificate configured. PSK is always assumed to be configured on server */
		/* or proxy because PSK can come from database. */

		if (NULL != ctx_psk)
		{
			if (NULL == (s->tls_ctx = SSL_new(ctx_psk)))
			{
				zbx_snprintf_alloc(error, &error_alloc, &error_offset, "cannot create context to accept"
						" connection:");
				zbx_tls_error_msg(error, &error_alloc, &error_offset);
				goto out;
			}
		}
		else
		{
			THIS_SHOULD_NEVER_HAPPEN;
			goto out;
		}
	}
	else if (0 != (tls_accept & ZBX_TCP_SEC_TLS_CERT))
	{
		if (NULL != ctx_cert)
		{
			if (NULL == (s->tls_ctx = SSL_new(ctx_cert)))
			{
				zbx_snprintf_alloc(error, &error_alloc, &error_offset, "cannot create context to accept"
						" connection:");
				zbx_tls_error_msg(error, &error_alloc, &error_offset);
				goto out;
			}
		}
		else
		{
			*error = zbx_dsprintf(*error, "not ready for certificate-based incoming connection: certificate"
					" not loaded");
			goto out;
		}
	}
	else	/* PSK */
	{
		if (NULL != ctx_psk)
		{
			if (NULL == (s->tls_ctx = SSL_new(ctx_psk)))
			{
				zbx_snprintf_alloc(error, &error_alloc, &error_offset, "cannot create context to accept"
						" connection:");
				zbx_tls_error_msg(error, &error_alloc, &error_offset);
				goto out;
			}
		}
		else
		{
			*error = zbx_dsprintf(*error, "not ready for PSK-based incoming connection: PSK not loaded");
			goto out;
		}
	}

	if (1 != SSL_set_fd(s->tls_ctx, s->socket))
	{
		*error = zbx_strdup(*error, "cannot set socket for TLS context");
		goto out;
	}

	zbx_log_ciphersuites(__function_name, s->tls_ctx);

	/* TLS handshake */

	info_buf[0] = '\0';	/* empty buffer for zbx_openssl_info_callback() messages */

	if (1 != (res = SSL_accept(s->tls_ctx)))
	{
		int	error_code;

		error_code = SSL_get_error(s->tls_ctx, res);

		if (0 == res)
		{
			zbx_snprintf_alloc(error, &error_alloc, &error_offset, "TLS connection has been closed during "
					"handshake:");
		}
		else
		{
			zbx_snprintf_alloc(error, &error_alloc, &error_offset, "TLS handshake returned error "
					"code %d:", error_code);
		}

		zbx_tls_error_msg(error, &error_alloc, &error_offset);
		zbx_snprintf_alloc(error, &error_alloc, &error_offset, ": %s", info_buf);
		goto out;
	}

	/* Is this TLS conection using certificate or PSK ? */

	cipher_name = SSL_get_cipher(s->tls_ctx);

	if (0 == strncmp("PSK-", cipher_name, 4))
	{
		s->connection_type = ZBX_TCP_SEC_TLS_PSK;
	}
	else if (0 != strncmp("NONE", cipher_name, 4))		/* TODO list explicitly certificate ciphers */
	{
		s->connection_type = ZBX_TCP_SEC_TLS_CERT;

		/* TODO log some certificate debug info here */

		/* validate peer certificate TODO: Issuer and Subject validation */
	}
	else
	{
		THIS_SHOULD_NEVER_HAPPEN;
		zbx_tls_close(s);
		return FAIL;
	}

	if (SUCCEED == zabbix_check_log_level(LOG_LEVEL_DEBUG))
	{
		zabbix_log(LOG_LEVEL_DEBUG, "End of %s(): SUCCEED (established %s %s)", __function_name,
				SSL_get_version(s->tls_ctx), cipher_name);
	}

	return SUCCEED;
out:
	if (NULL != s->tls_ctx)
	{
		SSL_free(s->tls_ctx);
		s->tls_ctx = NULL;
	}

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s:%s", __function_name, zbx_result_string(ret),
			ZBX_NULL2EMPTY_STR(*error));
	return ret;
#endif
}
#endif

/******************************************************************************
 *                                                                            *
 * Function: zbx_tls_close                                                    *
 *                                                                            *
 * Purpose: close a TLS connection before closing a TCP socket                *
 *                                                                            *
 ******************************************************************************/
void	zbx_tls_close(zbx_sock_t *s)
{
#if defined(HAVE_POLARSSL)
	if (NULL != s->tls_ctx)
	{
		ssl_close_notify(s->tls_ctx);
		ssl_free(s->tls_ctx);
		zbx_free(s->tls_ctx);
	}
#elif defined(HAVE_GNUTLS)
	int	res;

	if (NULL != s->tls_ctx)
	{
		/* shutdown TLS connection */
		while (GNUTLS_E_SUCCESS != (res = gnutls_bye(s->tls_ctx, GNUTLS_SHUT_RDWR)))
		{
			if (GNUTLS_E_INTERRUPTED == res || GNUTLS_E_AGAIN == res)
			{
				continue;
			}
			else
			{
				zabbix_log(LOG_LEVEL_WARNING, "gnutls_bye() returned: %d %s", res,
						gnutls_strerror(res));

				if (0 != gnutls_error_is_fatal(res))
					break;
			}
		}

		gnutls_credentials_clear(s->tls_ctx);
		gnutls_deinit(s->tls_ctx);
		s->tls_ctx = NULL;
	}

	if (NULL != s->tls_psk_client_creds)
	{
		gnutls_psk_free_client_credentials(s->tls_psk_client_creds);
		s->tls_psk_client_creds = NULL;
	}

	if (NULL != s->tls_psk_server_creds)
	{
		gnutls_psk_free_server_credentials(s->tls_psk_server_creds);
		s->tls_psk_server_creds = NULL;
	}
#elif defined(HAVE_OPENSSL)
	int	res;

	if (NULL != s->tls_ctx)
	{
		if (1 != (res = SSL_shutdown(s->tls_ctx)))
		{
			/* TODO add error processing */
		}

		SSL_free(s->tls_ctx);
		s->tls_ctx = NULL;
	}
#endif
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_tls_connection_type_name                                     *
 *                                                                            *
 * Purpose: translate connection type code to name                            *
 *                                                                            *
 ******************************************************************************/
const char	*zbx_tls_connection_type_name(unsigned int type)
{
	if (ZBX_TCP_SEC_UNENCRYPTED == type)
		return "unencrypted";
	else if (ZBX_TCP_SEC_TLS_CERT == type)
		return "TLS with certificate";
	else if (ZBX_TCP_SEC_TLS_PSK == type)
		return "TLS with PSK";
	else
		return "unknown";
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_tls_get_attr                                                 *
 *                                                                            *
 * Purpose: get connection type, certificate and PSK attributes from a        *
 *          context of established connection                                 *
 *                                                                            *
 ******************************************************************************/
int	zbx_tls_get_attr(const zbx_sock_t *s, zbx_tls_conn_attr_t *attr)
{
	attr->connection_type = s->connection_type;

#if defined(HAVE_POLARSSL)
	if (ZBX_TCP_SEC_TLS_CERT == s->connection_type)
	{
		const x509_crt	*peer_cert;
		int		res;

		if (NULL == (peer_cert = ssl_get_peer_cert(s->tls_ctx)))
		{
			zabbix_log(LOG_LEVEL_WARNING, "no peer certificate, ssl_get_peer_cert() returned NULL");
			return FAIL;
		}

		if (0 >= (res = x509_dn_gets(attr->issuer, sizeof(attr->issuer), &peer_cert->issuer)))
		{
			zabbix_log(LOG_LEVEL_WARNING, "cannot get issuer distinguished name, x509_dn_gets() returned "
					"%d", res);
			return FAIL;
		}

		if (0 >= (res = x509_dn_gets(attr->subject, sizeof(attr->subject), &peer_cert->subject)))
		{
			zabbix_log(LOG_LEVEL_WARNING, "cannot get subject distinguished name, x509_dn_gets() returned "
					"%d", res);
			return FAIL;
		}
	}
	else if (ZBX_TCP_SEC_TLS_PSK == s->connection_type)
	{
		attr->psk_identity = (char *)s->tls_ctx->psk_identity;
		attr->psk_identity_len = s->tls_ctx->psk_identity_len;
	}
#elif defined(HAVE_GNUTLS)
	if (ZBX_TCP_SEC_TLS_CERT == s->connection_type)
	{
		/* TODO implement getting pointers to issuer and subject and their lengths */
		return FAIL;
	}
	else if (ZBX_TCP_SEC_TLS_PSK == s->connection_type)
	{
		if (NULL != (attr->psk_identity = gnutls_psk_server_get_username(s->tls_ctx)))
			attr->psk_identity_len = strlen(attr->psk_identity);
		else
			return FAIL;
	}
#elif defined(HAVE_OPENSSL)
	if (ZBX_TCP_SEC_TLS_CERT == s->connection_type)
	{
		/* TODO implement getting pointers to issuer and subject and their lengths */
		return FAIL;
	}
	else if (ZBX_TCP_SEC_TLS_PSK == s->connection_type)
	{
		SSL_SESSION	*sess;

		if (NULL != (sess = SSL_get_session(s->tls_ctx)))
		{
			if (NULL != (attr->psk_identity = sess->psk_identity))
				attr->psk_identity_len = strlen(attr->psk_identity);
			else
				return FAIL;
		}
		else
			return FAIL;
	}
#endif
	return SUCCEED;
}

#if defined(_WINDOWS) && (defined(HAVE_POLARSSL) || defined(HAVE_GNUTLS) || defined(HAVE_OPENSSL))
/******************************************************************************
 *                                                                            *
 * Function: zbx_tls_pass_vars                                                *
 *                                                                            *
 * Purpose: pass some TLS variables from one thread to other                  *
 *                                                                            *
 * Comments: used in Zabbix sender on MS Windows                              *
 *                                                                            *
 ******************************************************************************/
void	zbx_tls_pass_vars(ZBX_THREAD_SENDVAL_TLS_ARGS *args)
{
#if defined(HAVE_POLARSSL)
	args->my_psk = my_psk;
	args->my_psk_len = my_psk_len;
	args->my_psk_identity = my_psk_identity;
	args->my_psk_identity_len = my_psk_identity_len;
	args->ca_cert = ca_cert;
	args->crl = crl;
	args->my_cert = my_cert;
	args->my_priv_key = my_priv_key;
	args->entropy = entropy;
	args->ctr_drbg = ctr_drbg;
	args->ciphersuites_cert = ciphersuites_cert;
	args->ciphersuites_psk = ciphersuites_psk;
#elif defined(HAVE_GNUTLS)
	args->my_cert_creds = my_cert_creds;
	args->my_psk_client_creds = my_psk_client_creds;
	args->ciphersuites_cert = ciphersuites_cert;
	args->ciphersuites_psk = ciphersuites_psk;
#elif defined(HAVE_OPENSSL)
	args->ctx_cert = ctx_cert;
	args->ctx_psk = ctx_psk;
	args->psk_identity_for_cb = psk_identity_for_cb;
	args->psk_identity_len_for_cb = psk_identity_len_for_cb;
	args->psk_for_cb = psk_for_cb;
	args->psk_len_for_cb = psk_len_for_cb;
#endif
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_tls_take_vars                                                *
 *                                                                            *
 * Purpose: pass some TLS variables from one thread to other                  *
 *                                                                            *
 * Comments: used in Zabbix sender on MS Windows                              *
 *                                                                            *
 ******************************************************************************/
void	zbx_tls_take_vars(ZBX_THREAD_SENDVAL_TLS_ARGS *args)
{
#if defined(HAVE_POLARSSL)
	my_psk = args->my_psk;
	my_psk_len = args->my_psk_len;
	my_psk_identity = args->my_psk_identity;
	my_psk_identity_len = args->my_psk_identity_len;
	ca_cert = args->ca_cert;
	crl = args->crl;
	my_cert = args->my_cert;
	my_priv_key = args->my_priv_key;
	entropy = args->entropy;
	ctr_drbg = args->ctr_drbg;
	ciphersuites_cert = args->ciphersuites_cert;
	ciphersuites_psk = args->ciphersuites_psk;
#elif defined(HAVE_GNUTLS)
	my_cert_creds = args->my_cert_creds;
	my_psk_client_creds = args->my_psk_client_creds;
	ciphersuites_cert = args->ciphersuites_cert;
	ciphersuites_psk = args->ciphersuites_psk;
#elif defined(HAVE_OPENSSL)
	ctx_cert = args->ctx_cert;
	ctx_psk = args->ctx_psk;
	psk_identity_for_cb = args->psk_identity_for_cb;
	psk_identity_len_for_cb = args->psk_identity_len_for_cb;
	psk_for_cb = args->psk_for_cb;
	psk_len_for_cb = args->psk_len_for_cb;
#endif
}
#endif
