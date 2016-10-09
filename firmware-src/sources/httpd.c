/*
 * httpd.h
 *
 * Copyright 2016 DeviceHive
 *
 * Author: Nikolay Khabarov
 *
 */

#include "httpd.h"

#include <ets_sys.h>
#include <osapi.h>
#include <user_interface.h>
#include <espconn.h>
#include <mem.h>
#include "dhdebug.h"
#include "dhap_pages.h"
#include "dhap_post.h"
#include "dhesperrors.h"
#include "dhsettings.h"
#include "dhstatistic.h"
#include "snprintf.h"

#define MAX_CONNECTIONS 5
#define HTTPD_PORT 80
#define POST_BUF_SIZE 2048

typedef struct {
	uint8 remote_ip[4];
	int remote_port;
	HTTP_CONTENT content;
} CONTENT_ITEM;

LOCAL struct espconn mHttpdConn;
LOCAL const char *mRedirectHost = 0;
LOCAL unsigned int mConnected = 0;
LOCAL struct espconn *mCurrentPost = 0;
LOCAL char *mPostBuf = 0;
LOCAL unsigned int mPostBufPos = 0;
LOCAL HttpRequestCb mGetHttpRequestCb = 0;
LOCAL HttpRequestCb mPostHttpRequestCb = 0;
LOCAL CONTENT_ITEM mContentQueue[MAX_CONNECTIONS] = {0};

LOCAL int ICACHE_FLASH_ATTR is_remote_equal(esp_tcp *tcp, CONTENT_ITEM *item) {
	if (os_memcmp(tcp->remote_ip, item->remote_ip, sizeof(tcp->remote_ip)) == 0
			&& tcp->remote_port == item->remote_port) {
		return 1;
	}
	return 0;
}

LOCAL void ICACHE_FLASH_ATTR send_res(struct espconn *conn, const char *data, int len) {
	sint8 res = espconn_send(conn, (char *)data, len);
	if(res) {
		dhstatistic_inc_network_errors_count();
		dhesperrors_espconn_result("Httpd espconn_send returned: %d", res);
	} else {
		dhstatistic_add_bytes_sent(len);
	}
}

LOCAL void ICACHE_FLASH_ATTR on_client_disconnect(struct espconn *conn) {
	int i;
	for(i = 0; i < MAX_CONNECTIONS; i++) {
		if(is_remote_equal(conn->proto.tcp, &mContentQueue[i])) {
			mContentQueue[i].remote_port = 0;
			break;
		}
	}
	mConnected--;
}

LOCAL void ICACHE_FLASH_ATTR dhap_httpd_disconnect_cb(void *arg) {
	struct espconn *conn = arg;
	on_client_disconnect(conn);
	dhdebug("Httpd client disconnected, %u left", mConnected);
}

LOCAL void ICACHE_FLASH_ATTR dhap_httpd_sent_cb(void *arg) {
	struct espconn *conn = arg;
	int i;
	for(i = 0; i < MAX_CONNECTIONS; i++) {
		if(is_remote_equal(conn->proto.tcp, &mContentQueue[i])) {
			send_res(conn, mContentQueue[i].content.data, mContentQueue[i].content.len);
			mContentQueue[i].remote_port = 0;
			return;
		}
	}
	espconn_disconnect(conn);
	dhdebug("Httpd data sent");
}

LOCAL HTTP_RESPONSE_STATUS ICACHE_FLASH_ATTR parse_request(
		const char *data, unsigned short len, HttpRequestCb cb, HTTP_CONTENT *content_out) {
	static const char content_length[] = "Content-Length:";
	static const char authorization[] = "Authorization:";
	static const char bearer[] = "Bearer";
	static const char sp[] = "\r\n\r\n";
	int i, j;

	i = 0;
	while(data[i] != ' ') // rewind method
		i++;
	while(data[i] == ' ')
		i++;
	j = i;
	while(data[j] != ' ') // find end of path
		j++;
	char *path = (char *)os_malloc(j - i + 1);
	if(path == 0)
		return HRCS_INTERNAL_ERROR;
	snprintf(path, j - i + 1, "%s", &data[i]);

	unsigned int cont_len = 0;
	char *key = 0;
	HTTP_RESPONSE_STATUS res = HRCS_INTERNAL_ERROR;
	for(i = j; i < len; i++) {
		if(os_strncmp(&data[i], content_length, sizeof(content_length) - 1) == 0) {
			i += sizeof(content_length) - 1;
			while(data[i] == ' ')
				i++;
			if(strToUInt(&data[i], &cont_len) == 0) {
				res = HRCS_BAD_REQUEST;
				break;
			}
		} else if(os_strncmp(&data[i], authorization, sizeof(authorization) - 1) == 0) {
			i += sizeof(authorization) - 1;
			while(data[i] == ' ')
				i++;
			if(os_strncmp(&data[i], bearer, sizeof(bearer) - 1) == 0) {
				i += sizeof(bearer) - 1;
				while(data[i] == ' ')
					i++;
				j = i;
				while(data[j] != '\r')
					j++;
				key = (char *)os_malloc(j - i + 1);
				if(key == 0) {
					res = HRCS_INTERNAL_ERROR;
					break;
				}
				snprintf(key, j - i + 1, "%s", &data[i]);
				i = j - 1;
			}
		} else if(os_strncmp(&data[i], sp, sizeof(sp) - 1) == 0) {
			i += sizeof(sp) - 1;
			if(cont_len && cont_len > len - i) {
				res = HRCS_NOT_FINISHED;
				break;
			}
			HTTP_CONTENT in;
			in.data = cont_len ? &data[i] : 0;
			in.len = cont_len;
			res = cb(path, key, &in, content_out);
			break;
		}
	}
	os_free(path);
	if(key)
		os_free(key);
	return res;
}

LOCAL HTTP_RESPONSE_STATUS ICACHE_FLASH_ATTR receive_post(
		const char *data, unsigned short len, HTTP_CONTENT *content_out) {
	if(mPostBuf == 0) {
		mPostBuf = (char*)os_malloc(POST_BUF_SIZE);
		if(mPostBuf == 0) {
			mCurrentPost = 0;
			return HRCS_INTERNAL_ERROR;
		}
	}
	if(len > POST_BUF_SIZE - mPostBufPos) {
		mCurrentPost = 0;
		return HRCS_INTERNAL_ERROR;
	}
	os_memcpy(&mPostBuf[mPostBufPos], data, len);
	mPostBufPos += len;
	HTTP_RESPONSE_STATUS res = parse_request(mPostBuf, mPostBufPos, mPostHttpRequestCb, content_out);
	if(res != HRCS_NOT_FINISHED && mCurrentPost) {
		mCurrentPost = 0;
		os_free(mPostBuf);
		mPostBuf = 0;
	}
	return res;
}

LOCAL void ICACHE_FLASH_ATTR dhap_httpd_recv_cb(void *arg, char *data, unsigned short len) {
	struct espconn *conn = arg;
	static const char get[] = "GET ";
	static const char post[] = "POST ";
	static const char host[] = "Host:";
	static char internal[] = "HTTP/1.0 500 No Memory\r\nContent-Type: text/plain\r\nContent-Length: 14\r\n\r\nInternal Error";
	static char notfound[] = "HTTP/1.0 404 Not Found\r\nContent-Type: text/plain\r\nContent-Length: 9\r\n\r\nNot Found";
	static char notimplemented[] = "HTTP/1.0 501 Not Implemented\r\nContent-Type: text/plain\r\nContent-Length: 15\r\n\r\nNot Implemented";
	static char unauthorized[] = "HTTP/1.0 401 Unauthorized\r\nContent-Type: text/html; charset=UTF-8\r\nContent-Length: 12\r\n\r\nUnauthorized";
	static char badrequest[] = "HTTP/1.0 400 Bad Request\r\nContent-Length:11\r\n\r\nBad Request";
	static char redirectresponse[] = "HTTP/1.0 302 Moved\r\nContent-Length: 0\r\nLocation: http://%s\r\n\r\n";
	static char ok[] = "HTTP/1.0 200 OK\r\nContent-Type: text/html; charset=UTF-8\r\nContent-Length: %u\r\n\r\n";
	static char no_content[] = "HTTP/1.0 204 No content\r\nContent-Type: text/html; charset=UTF-8\r\nContent-Length: 0\r\n\r\n";
	HTTP_CONTENT content_out;
	content_out.len = 0;
	dhdebug("Httpd received %d bytes", len);
	dhstatistic_add_bytes_received(len);
	HTTP_RESPONSE_STATUS res = HRCS_INTERNAL_ERROR;

	if(conn == mCurrentPost) {
		res = receive_post(data, len, &content_out);
	} else {
		if(mRedirectHost) {
			const int redirect_host_len = os_strlen(mRedirectHost);
			const int to = (int)len - redirect_host_len - sizeof(host) + 1;
			int i;
			for(i = 0; i < to; i++) {
				if(os_strncmp(&data[i], host, sizeof(host) - 1) == 0) {
					i += sizeof(host) - 1;
					while(data[i] == ' ')
						i++;
					if(os_strncmp(&data[i], mRedirectHost, redirect_host_len) != 0) {
						dhdebug("Httpd redirect");
						char redirect[sizeof(redirectresponse) - 2 + redirect_host_len];
						snprintf(redirect, sizeof(redirect), redirectresponse, mRedirectHost);
						send_res(conn, redirect, sizeof(redirect) - 1);
						dhstatistic_inc_httpd_requests_count();
						return;
					}
					break;
				}
			}
		}

		if(len < sizeof(get) || len < sizeof(post)) {
			res = HRCS_BAD_REQUEST;
		} else if(os_strncmp(data, post, sizeof(post) - 1) == 0) {
			if(mCurrentPost) {
				espconn_disconnect(mCurrentPost);
				dhdebug("Httpd new POST sender, refuse old");
			}
			mCurrentPost = conn;
			mPostBufPos = 0;
			res = receive_post(data, len, &content_out);
		} else if(os_strncmp(data, get, sizeof(get) - 1) == 0) {
			res = parse_request(data, len, mGetHttpRequestCb, &content_out);
		} else {
			res = HRCS_NOT_IMPLEMENTED;
		}
	}

	if (res != HRCS_NOT_FINISHED) {
		dhstatistic_inc_httpd_requests_count();
	}

	switch (res) {
	case HRCS_ANSWERED:
	{
		if(content_out.len == 0) {
			send_res(conn, no_content, sizeof(no_content) - 1);
			return;
		}
		int i;
		CONTENT_ITEM *item = 0;
		for(i = 0; i < MAX_CONNECTIONS; i++) {
			if(is_remote_equal(conn->proto.tcp, &mContentQueue[i])) {
				dhdebug("Httpd duplicate responses");
				send_res(conn, internal, sizeof(internal) - 1);
				dhstatistic_inc_httpd_errors_count();
				return;
			} else if(mContentQueue[i].remote_port == 0) {
				item = &mContentQueue[i];
			}
		}
		if(item == 0) {
			dhdebug("Httpd no place for responses");
			send_res(conn, internal, sizeof(internal) - 1);
			dhstatistic_inc_httpd_errors_count();
			return;
		}
		char response[sizeof(ok) + 8];
		int response_len = snprintf(response, sizeof(response), ok, content_out.len);
		if(content_out.len) {
			os_memcpy(item->remote_ip, conn->proto.tcp->remote_ip, sizeof(item->remote_ip));
			item->remote_port = conn->proto.tcp->remote_port;
			item->content.data = content_out.data;
			item->content.len = content_out.len;
		}
		send_res(conn, response, response_len);
		return;
	}
	case HRCS_NOT_FINISHED:
		return;
	case HRCS_BAD_REQUEST:
		dhdebug("Httpd bad request");
		send_res(conn, badrequest, sizeof(badrequest) - 1);
		break;
	case HRCS_NOT_FOUND:
		dhdebug("Httpd not found");
		send_res(conn, notfound, sizeof(notfound) - 1);
		break;
	case HRCS_NOT_IMPLEMENTED:
		dhdebug("Httpd not implemented");
		send_res(conn, notimplemented, sizeof(notimplemented) - 1);
		break;
	case HRCS_UNAUTHORIZED:
		dhdebug("Httpd unauthorized");
		send_res(conn, unauthorized, sizeof(unauthorized) - 1);
		break;
	case HRCS_INTERNAL_ERROR:
	default:
		dhdebug("Httpd internal error");
		send_res(conn, internal, sizeof(internal) - 1);
	}
	dhstatistic_inc_httpd_errors_count();
}

LOCAL void ICACHE_FLASH_ATTR dhap_httpd_reconnect_cb(void *arg, sint8 err) {
	struct espconn *conn = arg;
	on_client_disconnect(conn);
	dhdebug("Httpd connection error occurred %d", err);
}

LOCAL void ICACHE_FLASH_ATTR dhap_httpd_connect_cb(void *arg) {
	struct espconn *conn = arg;
	mConnected++;
	if(mConnected > MAX_CONNECTIONS) {
		espconn_disconnect(conn);
		dhdebug("Httpd refuse connection, already %u connections", mConnected);
		return;
	}
	espconn_regist_recvcb(conn, dhap_httpd_recv_cb);
	espconn_regist_disconcb(conn, dhap_httpd_disconnect_cb);
	espconn_regist_sentcb(conn, dhap_httpd_sent_cb);
	dhdebug("Httpd client connected");
}

void ICACHE_FLASH_ATTR httpd_init(HttpRequestCb get_cb, HttpRequestCb post_cb) {
	mGetHttpRequestCb = get_cb;
	mPostHttpRequestCb = post_cb;
	static esp_tcp httpdTcp;
	os_memset(&httpdTcp, 0, sizeof(httpdTcp));
	os_memset(&mHttpdConn, 0, sizeof(mHttpdConn));
	mHttpdConn.type = ESPCONN_TCP;
	mHttpdConn.state = ESPCONN_NONE;
	httpdTcp.local_port = HTTPD_PORT;
	mHttpdConn.proto.tcp = &httpdTcp;

	espconn_regist_connectcb(&mHttpdConn, dhap_httpd_connect_cb);
	espconn_regist_reconcb(&mHttpdConn, dhap_httpd_reconnect_cb);
	sint8 res = espconn_accept(&mHttpdConn);
	if(res)
		dhdebug("espconn_accept returned: %d", res);
	else
		dhdebug("Httpd started");
}

void httpd_redirect(const char *host) {
	mRedirectHost = host;
}